// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
extern "C" bool ps2_vif1_on_worker(); // PERF-HN (ps2_memory.cpp)
extern "C" void ps2_vif1_mark_worker_thread();
extern "C" void ps2_gs_async_batch(bool on);
extern bool g_ps2Ft2On; extern "C" void ps2_ft2_ev(uint32_t type, uint64_t arg); // PROBE-FT2
extern "C" bool ps2_fk_lookup(uint32_t chainSize, uint32_t pos, uint32_t *outSrc, uint32_t *outBytes, uint32_t *outBufOff);
extern "C" void ps2_scanCameraRegion(const char *caller, uint64_t callIndex, uint32_t ra);
extern "C" void ps2_fe_recordUnpack(uint32_t qw, uint32_t ev, uint8_t cmd, uint16_t dest, uint16_t num,
                                    uint8_t cl, uint8_t wl, uint8_t flg, uint16_t tops, uint32_t src_ea);
#include "probe_ae.h"
#include "ThreadNaming.h"
#include <cstring>
#include <immintrin.h>
#include <cstdio>
#include <chrono>
#include <map>
#include <algorithm>
#include <tuple>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <functional>
#include <cstdarg>
#include <bitset>
#include <array>

extern void ps2xSafeLog(const char *fmt, ...);
extern bool ps2xVxOn(); extern void ps2xVxMarkThread(); extern void ps2xVxIdle(uint64_t); extern void ps2xVxEv(uint32_t, uint64_t); // PROBE-VX
extern void ps2xVu1Path1Batch(bool); extern void ps2xVu1FlushPath1(); // PERF-V1I

// PROBE-VW: Vif1Worker (VIF1 parser thread) time split (PS2X_VW=1). Plain counters: only the Vif1Worker thread writes them.
namespace vw // PROBE-VW
{
inline bool on() // PROBE-VW
{
    static const bool v = []() { const char *e = std::getenv("PS2X_VW"); return e && e[0] == '1'; }();
    return v;
}
enum { kCDirect = 32, kCMscal, kCMpg, kCState, kCNop, kCMark, kCOther, kCN }; // PROBE-VW (0..31 = unpack idx|masked<<4)
struct St // PROBE-VW
{
    uint64_t t[kCN] = {}, n[kCN] = {}, qw[kCN] = {};
    uint64_t blkRing = 0, blkDrain = 0, outside = 0, idle = 0, proc = 0, jobs = 0;
    uint64_t tsc0 = 0;
    std::chrono::steady_clock::time_point t0{};
};
static St g; // PROBE-VW
inline uint32_t cls(uint8_t op) // PROBE-VW
{
    if ((op & 0x60) == 0x60)
        return (op & 0x0Fu) | ((op & 0x10u) ? 16u : 0u);
    switch (op)
    {
    case 0x50: case 0x51: return kCDirect;
    case 0x14: case 0x15: case 0x17: return kCMscal;
    case 0x4A: return kCMpg;
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x10: case 0x11: case 0x13: case 0x20: case 0x30: case 0x31: return kCState;
    case 0x00: return kCNop;
    case 0x07: return kCMark;
    default: return kCOther;
    }
}
static const char *const kUN[16] = {"S-32", "S-16", "S-8", "S?", "V2-32", "V2-16", "V2-8", "V2?", "V3-32", "V3-16", "V3-8", "V3?", "V4-32", "V4-16", "V4-8", "V4-5"}; // PROBE-VW
struct Iter // PROBE-VW: one VIF code, timed from decode to the end of its loop iteration (blocked time carved out)
{
    uint64_t t = 0, blk0 = 0;
    uint8_t op = 0;
    uint8_t num = 0;
    uint16_t imm = 0;
    Iter(bool en, uint8_t o, uint8_t nm, uint16_t im) : op(o), num(nm), imm(im)
    {
        if (en)
        {
            blk0 = g.blkRing + g.blkDrain;
            t = __rdtsc();
        }
    }
    ~Iter()
    {
        if (!t)
            return;
        uint64_t d = __rdtsc() - t;
        const uint64_t b = g.blkRing + g.blkDrain - blk0;
        d = d > b ? d - b : 0;
        const uint32_t c = cls(op);
        g.t[c] += d;
        ++g.n[c];
        if (c < 32u)
            g.qw[c] += num ? num : 256u;
        else if (c == kCDirect)
            g.qw[c] += imm ? imm : 65536u;
    }
};
} // namespace vw
inline void vwBlk(bool drain, uint64_t t0) { if (drain) vw::g.blkDrain += __rdtsc() - t0; else vw::g.blkRing += __rdtsc() - t0; } // PROBE-VW
// PROBE-VW: called by vif1Worker (ps2_memory.cpp) once per job; prints every ~5 s
bool ps2xVwOn() { return vw::on(); } // PROBE-VW
void ps2xVwJob(uint64_t idle, uint64_t outside, uint64_t proc) // PROBE-VW
{
    using namespace vw;
    St &s = g;
    s.idle += idle;
    s.outside += outside;
    s.proc += proc;
    ++s.jobs;
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
    const double fr = s.jobs ? static_cast<double>(s.jobs) : 1.0;
    const double k = ms / fr;
    uint64_t un = 0, unQw = 0;
    uint32_t ord[32];
    for (uint32_t i = 0; i < 32; ++i)
    {
        ord[i] = i;
        un += s.t[i];
        unQw += s.qw[i];
    }
    std::sort(ord, ord + 32, [&](uint32_t a, uint32_t b) { return s.t[a] > s.t[b]; });
    char ub[400];
    int n = 0;
    ub[0] = 0;
    for (int i = 0; i < 5 && n < 340; ++i)
    {
        const uint32_t c = ord[i];
        if (!s.t[c])
            break;
        n += std::snprintf(ub + n, sizeof(ub) - n, " %s%s %.2f(%.0f,%.0fqw)", kUN[c & 15u], (c & 16u) ? "m" : "", s.t[c] * k, s.n[c] / fr, s.qw[c] / fr);
    }
    const uint64_t blk = s.blkRing + s.blkDrain;
    const uint64_t busy = s.proc > blk ? s.proc - blk : 0;
    const uint64_t classified = un + s.t[kCDirect] + s.t[kCMscal];
    const uint64_t other = busy > classified ? busy - classified : 0;
    ::ps2xSafeLog("[vw] frames=%llu ms/frame: busy=%.2f | unpack=%.2f (%.0f qw/f;%s) direct=%.2f(%.0f,%.0fqw) mscal=%.2f(%.0f) mpg=%.2f(%.0f) state=%.2f(%.0f) nop=%.2f mark=%.2f other=%.2f | blocked: ringfull=%.2f drain=%.2f | outside=%.2f idle=%.2f\n",
                  (unsigned long long)s.jobs, busy * k, un * k, unQw / fr, ub, s.t[kCDirect] * k, s.n[kCDirect] / fr, s.qw[kCDirect] / fr,
                  s.t[kCMscal] * k, s.n[kCMscal] / fr, s.t[kCMpg] * k, s.n[kCMpg] / fr, s.t[kCState] * k, s.n[kCState] / fr, s.t[kCNop] * k, s.t[kCMark] * k,
                  other * k, s.blkRing * k, s.blkDrain * k, s.outside * k, s.idle * k);
    std::fflush(stderr);
    s = St{};
    s.t0 = now;
    s.tsc0 = tn;
}

// =========================================================================
// Task DZ: VIF1 UNPACK ring buffer & VIF1 register dump
// =========================================================================
struct DzUnpackEntry {
    uint8_t cmd;
    uint8_t vn;
    uint8_t vl;
    uint8_t usn;
    uint8_t flg;
    uint8_t cl;
    uint8_t wl;
    uint8_t num;
    uint32_t dest;
    uint32_t src;
    uint8_t fast;
};

static std::mutex s_dzUnpackMutex;
static DzUnpackEntry s_dzUnpackRing[256];
static uint32_t s_dzUnpackHead = 0;
static uint32_t s_dzUnpackTotal = 0;

static void ps2_vif1_recordDzUnpack(uint8_t cmd, uint8_t vn, uint8_t vl, uint8_t usn, uint8_t flg,
                                    uint8_t cl, uint8_t wl, uint8_t num, uint32_t dest,
                                    uint32_t src, uint8_t isFast)
{
    std::lock_guard<std::mutex> lock(s_dzUnpackMutex);
    uint32_t idx = s_dzUnpackHead & 255u;
    s_dzUnpackRing[idx] = {cmd, vn, vl, usn, flg, cl, wl, num, dest, src, isFast};
    s_dzUnpackHead++;
    s_dzUnpackTotal++;
}

extern "C" void ps2_vif1_dumpDzRingCount(uint32_t maxCount, const char *prefix) {
    std::lock_guard<std::mutex> lock(s_dzUnpackMutex);
    uint32_t total = s_dzUnpackTotal;
    uint32_t count = std::min<uint32_t>(maxCount, total);
    uint32_t startIdx = total - count;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &e = s_dzUnpackRing[(startIdx + i) & 255u];
        std::printf("%s cmd=0x%02X vn=%u vl=%u usn=%u flg=%u cl=%u wl=%u num=%u dest=0x%03X src=0x%08X mode=%s\n",
                    prefix, e.cmd, e.vn, e.vl, e.usn, e.flg, e.cl, e.wl, e.num, e.dest, e.src,
                    e.fast ? "fast" : "generic");
    }
}

extern "C" void ps2_vif1_dumpDzRing() {
    ps2_vif1_dumpDzRingCount(256u, "[probe] dc15 unpack");
}

extern "C" bool ps2_vif1_findLastUnpackForQw(uint32_t qw, uint32_t *outDest, uint32_t *outNum, uint32_t *outSrc) {
    std::lock_guard<std::mutex> lock(s_dzUnpackMutex);
    uint32_t total = s_dzUnpackTotal;
    uint32_t count = std::min<uint32_t>(256u, total);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (s_dzUnpackHead - 1u - i) & 255u;
        const auto &e = s_dzUnpackRing[idx];
        uint32_t numQw = e.num ? static_cast<uint32_t>(e.num) : 256u;
        if (qw >= e.dest && qw < e.dest + numQw) {
            if (outDest) *outDest = e.dest;
            if (outNum) *outNum = e.num;
            if (outSrc) *outSrc = e.src;
            return true;
        }
    }
    return false;
}
// =========================================================================
// Task EH: VU1 unpack ring for qw 440..720 & shape counters
// =========================================================================
static const bool g_vu3dProbeEnabled = []() {
    const char *v = std::getenv("PS2X_PROBE_VU3D");
    return (v && (v[0] == '1' || v[0] == 't'));
}();

static inline bool isVu3dProbeEnabled()
{
    return g_vu3dProbeEnabled;
}

struct EhUnpackRecord {
    uint8_t cmd;
    uint8_t vn;
    uint8_t vl;
    uint8_t usn;
    uint8_t flg;
    uint8_t cl;
    uint8_t wl;
    uint8_t num;
    uint32_t dest;
    uint32_t src;
    uint8_t fast;
    uint8_t srcVec0[16];
    uint8_t srcVec1[16];
    uint32_t destQw0[4];
    uint32_t mask;
    uint32_t row[4];
    uint32_t col[4];
};

static std::mutex s_ehUnpackMutex;
static EhUnpackRecord s_ehUnpackRing[200];
static uint32_t s_ehUnpackHead = 0;
static uint32_t s_ehUnpackTotal = 0;

static std::atomic<uint64_t> s_ehHandledFalse[4][4][2]{};
static std::atomic<uint64_t> s_ehFastUsed[4][4][2]{};
static std::atomic<bool> s_ehShapeSeen[4][4][2]{};

// =========================================================================
// Task EN/EO/EP: VIF1 UNPACK source tracking & Event Ring Buffer
// =========================================================================
static std::atomic<uint32_t> s_eoUnpackBadCount{0};

extern "C" uint32_t ps2_vif1_getEoUnpackBadCount() {
    return s_eoUnpackBadCount.load(std::memory_order_relaxed);
}

// =========================================================================
// Task EQ: UNPACK V4-32 bad constant detection & RDRAM source tracking
// =========================================================================
static std::atomic<uint32_t> s_eqTotalV4_32{0};
static std::atomic<uint32_t> s_eqBadCount{0};
static std::atomic<uint32_t> s_eqPrintCount{0};
static std::atomic<uint32_t> s_eqRdramPrintCount{0};
static std::chrono::steady_clock::time_point s_eqLastCountsTime{};

// =========================================================================
// Task ER: UNPACK drop / taken tracking & chunk diagnostics
// =========================================================================
static std::atomic<uint32_t> s_erDropped[128]{};
static std::atomic<uint32_t> s_erTaken[128]{};
static std::atomic<uint32_t> s_erDropPrintCount{0};
static std::atomic<uint32_t> s_erChunkReportCount{0};
static std::chrono::steady_clock::time_point s_erLastCountsTime{};

static const uint8_t *s_erCurrentChunk = nullptr;
static uint32_t s_erCurrentChunkSize = 0;
static bool s_erCurrentChunkReported = false;

// =========================================================================
// Task EU/EV: Snapshot buffers for UNPACK 0x6C dest=235 num=10
// =========================================================================
static std::atomic<uint32_t> s_euUnpackCount{0};
extern "C" uint32_t ps2_vif1_getEuMatchedCount() {
    return s_euUnpackCount.load(std::memory_order_relaxed);
}

static uint32_t s_evSrc10[10][4]{};
static uint32_t s_evDst10[10][4]{};
static uint32_t s_evEffDest = 0;
static uint32_t s_evSrcRdram = 0xFFFFFFFFu;
static bool s_evPathFast = false;
static std::atomic<uint32_t> s_evN235{0};
static std::mutex s_evMutex;

extern "C" void ps2_vif1_dumpEvBuffers()
{
    std::lock_guard<std::mutex> lock(s_evMutex);
    uint32_t n = s_evN235.load(std::memory_order_relaxed);
    const char *pathStr = s_evPathFast ? "fast" : "generic";
    std::printf("[probe] ev info n235=%u eff=%u src=0x%08X path=%s\n", n, s_evEffDest, s_evSrcRdram, pathStr);
    for (uint32_t i = 0; i < 10u; ++i)
    {
        std::printf("[probe] ev src10 n=%u i=%u %08X %08X %08X %08X\n",
                    n, i, s_evSrc10[i][0], s_evSrc10[i][1], s_evSrc10[i][2], s_evSrc10[i][3]);
    }
    for (uint32_t i = 0; i < 10u; ++i)
    {
        std::printf("[probe] ev dst10 path=%s i=%u %08X %08X %08X %08X\n",
                    pathStr, i, s_evDst10[i][0], s_evDst10[i][1], s_evDst10[i][2], s_evDst10[i][3]);
    }
    std::fflush(stdout);
}

static std::mutex s_epMutex;
static char s_epRing[128][256];
static uint64_t s_epTotalEvents = 0;

extern "C" void ps2x_probeEvent(const char *msg)
{
    if (!isVu3dProbeEnabled() || !msg) return;
    std::lock_guard<std::mutex> lock(s_epMutex);
    uint32_t idx = static_cast<uint32_t>(s_epTotalEvents % 128ULL);
    std::strncpy(s_epRing[idx], msg, sizeof(s_epRing[idx]) - 1);
    s_epRing[idx][sizeof(s_epRing[idx]) - 1] = '\0';
    s_epTotalEvents++;
}

extern "C" void ps2x_probeDump()
{
    std::lock_guard<std::mutex> lock(s_epMutex);
    uint64_t total = s_epTotalEvents;
    uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(128ULL, total));
    uint64_t start = (total > 128ULL) ? (total - 128ULL) : 0ULL;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t idx = static_cast<uint32_t>((start + i) % 128ULL);
        std::printf("[probe] ep ev %u: %s\n", i, s_epRing[idx]);
    }
    std::fflush(stdout);
}

static void recordEpEvent(const char *fmt, ...)
{
    if (!isVu3dProbeEnabled()) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ps2x_probeEvent(buf);
}

static void ps2_vif1_recordEhUnpack(uint8_t cmd, uint8_t vn, uint8_t vl, uint8_t usn, uint8_t flg,
                                    uint8_t cl, uint8_t wl, uint8_t num, uint32_t dest,
                                    uint32_t src, uint8_t isFast,
                                    const uint8_t *src0, const uint8_t *src1,
                                    const uint32_t *d0,
                                    uint32_t mask, const uint32_t row[4], const uint32_t col[4])
{
    std::lock_guard<std::mutex> lock(s_ehUnpackMutex);
    uint32_t idx = s_ehUnpackHead % 200u;
    auto &e = s_ehUnpackRing[idx];
    e.cmd = cmd;
    e.vn = vn;
    e.vl = vl;
    e.usn = usn;
    e.flg = flg;
    e.cl = cl;
    e.wl = wl;
    e.num = num;
    e.dest = dest;
    e.src = src;
    e.fast = isFast;
    if (src0) std::memcpy(e.srcVec0, src0, 16); else std::memset(e.srcVec0, 0, 16);
    if (src1) std::memcpy(e.srcVec1, src1, 16); else std::memset(e.srcVec1, 0, 16);
    if (d0) std::memcpy(e.destQw0, d0, 16); else std::memset(e.destQw0, 0, 16);
    e.mask = mask;
    std::memcpy(e.row, row, 16);
    std::memcpy(e.col, col, 16);
    s_ehUnpackHead = (s_ehUnpackHead + 1u) % 200u;
    s_ehUnpackTotal++;
}

extern "C" void ps2_vif1_dumpEhRing() {
    std::lock_guard<std::mutex> lock(s_ehUnpackMutex);
    uint32_t total = s_ehUnpackTotal;
    uint32_t count = std::min<uint32_t>(200u, total);
    uint32_t startIdx = (total >= 200u) ? s_ehUnpackHead : 0u;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &e = s_ehUnpackRing[(startIdx + i) % 200u];
        char s0[33], s1[33];
        for (int b = 0; b < 16; ++b) {
            std::snprintf(s0 + b * 2, 3, "%02X", e.srcVec0[b]);
            std::snprintf(s1 + b * 2, 3, "%02X", e.srcVec1[b]);
        }
        std::printf("[probe] eh unpack cmd=0x%02X vn=%u vl=%u usn=%u flg=%u cl=%u wl=%u num=%u dest=0x%03X src=0x%08X mode=%s "
                    "s0=%s s1=%s d0=%08X %08X %08X %08X mask=0x%08X row=%08X %08X %08X %08X col=%08X %08X %08X %08X\n",
                    e.cmd, e.vn, e.vl, e.usn, e.flg, e.cl, e.wl, e.num, e.dest, e.src,
                    e.fast ? "fast" : "generic",
                    s0, s1,
                    e.destQw0[0], e.destQw0[1], e.destQw0[2], e.destQw0[3],
                    e.mask,
                    e.row[0], e.row[1], e.row[2], e.row[3],
                    e.col[0], e.col[1], e.col[2], e.col[3]);
    }
}

extern "C" void ps2_vif1_dumpEhShapes() {
    for (uint32_t v_n = 0; v_n < 4; ++v_n) {
        for (uint32_t v_l = 0; v_l < 4; ++v_l) {
            for (uint32_t u = 0; u < 2; ++u) {
                if (s_ehShapeSeen[v_n][v_l][u].load(std::memory_order_relaxed)) {
                    uint64_t hf = s_ehHandledFalse[v_n][v_l][u].load(std::memory_order_relaxed);
                    uint64_t f = s_ehFastUsed[v_n][v_l][u].load(std::memory_order_relaxed);
                    std::printf("[probe] eh shapes vn=%u vl=%u usn=%u handled_false=%llu fast=%llu\n",
                                v_n, v_l, u, static_cast<unsigned long long>(hf), static_cast<unsigned long long>(f));
                }
            }
        }
    }
}


extern void recordCompletedIntcCause(uint32_t cause);

namespace probe_ae {
    void onMovieKickMscalCount(uint32_t count);
}

static uint32_t count0x48MscalsInKick(const uint8_t *data, uint32_t sizeBytes) {
    uint32_t count = 0;
    uint32_t pos = 0;
    uint32_t cycle = 0;
    while (pos + 4 <= sizeBytes) {
        uint32_t cmd;
        std::memcpy(&cmd, data + pos, 4);
        pos += 4;
        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        if (opcode == 0) continue;
        if (opcode == 0x01) { cycle = imm; continue; }
        if (opcode == 0x02 || opcode == 0x03 || opcode == 0x04 || opcode == 0x05 || opcode == 0x07) continue;
        if (opcode == 0x10 || opcode == 0x11 || opcode == 0x13) continue;
        if (opcode == 0x14 || opcode == 0x15) {
            if ((imm * 8u) == 0x048u) {
                count++;
            }
            continue;
        }
        if (opcode == 0x17) continue;
        if (opcode == 0x20) { pos += 4; continue; }
        if (opcode == 0x30 || opcode == 0x31) { pos += 16; continue; }
        if (opcode == 0x4A) {
            uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            pos += instructionCount * 8u;
            continue;
        }
        if (opcode == 0x50 || opcode == 0x51) {
            uint32_t qw = (imm == 0u) ? 65536u : static_cast<uint32_t>(imm);
            pos += qw * 16u;
            continue;
        }
        if ((opcode & 0x60) == 0x60) {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl) {
            case 0: bitsPerComponent = 32; break;
            case 1: bitsPerComponent = 16; break;
            case 2: bitsPerComponent = 8; break;
            case 3: bitsPerComponent = (vn == 3) ? 4 : 16; break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = cycle & 0xFFu;
            uint32_t wl = (cycle >> 8) & 0xFFu;
            if (cl == 0u && wl == 0u) { cl = 1u; wl = 1u; }
            else if (wl == 0u) { wl = 1u; }
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl) {
                uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t rem = writeVectorCount % wl;
                if (rem > cl) rem = cl;
                sourceVectorCount = fullBlocks * cl + rem;
            }
            uint32_t totalBytes = (sourceVectorCount * bytesPerVector + 3u) & ~3u;
            pos += totalBytes;
            continue;
        }
        break;
    }
    return count;
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u && wl == 0u)
            {
                cl = 1u;
                wl = 1u;
            }
            else if (wl == 0u)
            {
                wl = 1u;
            }
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

// PERF-UV: UNPACK writes for cl >= wl in one SIMD loop. Every destination lane is exactly one of decoded / constant (row, col) / protected
// (keeps the old VU data word), selected per write-cycle position by the masks the caller derives from the same act[][] table as the scalar loop.
namespace
{
    template <int VL, int VN, bool ZX>
    inline __m128i vifDecode(const uint8_t *p)
    {
        if constexpr (VL == 0)
        {
            if constexpr (VN == 0)
            {
                uint32_t s;
                std::memcpy(&s, p, 4);
                return _mm_set1_epi32(static_cast<int>(s));
            }
            else if constexpr (VN == 1)
                return _mm_loadl_epi64(reinterpret_cast<const __m128i *>(p));
            else if constexpr (VN == 2)
            {
                uint32_t w[3];
                std::memcpy(w, p, 12);
                return _mm_set_epi32(0, static_cast<int>(w[2]), static_cast<int>(w[1]), static_cast<int>(w[0]));
            }
            else
                return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
        }
        else if constexpr (VL == 1)
        {
            if constexpr (VN == 0)
            {
                uint16_t r;
                std::memcpy(&r, p, 2);
                return _mm_set1_epi32(ZX ? static_cast<int>(r) : static_cast<int>(static_cast<int16_t>(r)));
            }
            else
            {
                uint64_t t = 0;
                std::memcpy(&t, p, VN == 1 ? 4 : (VN == 2 ? 6 : 8));
                const __m128i v = _mm_cvtsi64_si128(static_cast<long long>(t));
                return ZX ? _mm_cvtepu16_epi32(v) : _mm_cvtepi16_epi32(v);
            }
        }
        else
        {
            if constexpr (VN == 0)
                return _mm_set1_epi32(ZX ? static_cast<int>(p[0]) : static_cast<int>(static_cast<int8_t>(p[0])));
            else
            {
                uint32_t t = 0;
                std::memcpy(&t, p, VN == 1 ? 2 : (VN == 2 ? 3 : 4));
                const __m128i v = _mm_cvtsi32_si128(static_cast<int>(t));
                return ZX ? _mm_cvtepu8_epi32(v) : _mm_cvtepi8_epi32(v);
            }
        }
    }

    template <int VL, int VN, bool ZX>
    void vifUnpackLoop(uint8_t *vuData, const uint8_t *src, uint32_t count, uint32_t destVec, uint32_t cl, uint32_t wl, uint32_t bytesPerVector,
                       const __m128i *dm, const __m128i *cv, const __m128i *pm)
    {
        uint32_t cyclePos = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const __m128i dec = vifDecode<VL, VN, ZX>(src);
            const uint32_t c = (cyclePos > 3u) ? 3u : cyclePos;
            __m128i *d = reinterpret_cast<__m128i *>(vuData) + destVec;
            const __m128i old = _mm_loadu_si128(d);
            _mm_storeu_si128(d, _mm_or_si128(_mm_or_si128(_mm_and_si128(dec, dm[c]), cv[c]), _mm_and_si128(old, pm[c])));
            src += bytesPerVector;
            ++cyclePos;
            ++destVec;
            if (cyclePos == wl)
            {
                cyclePos = 0;
                destVec += (cl - wl);
            }
        }
    }

    template <int VL, int VN>
    void vifUnpackZx(bool zx, uint8_t *vuData, const uint8_t *src, uint32_t count, uint32_t destVec, uint32_t cl, uint32_t wl, uint32_t bpv,
                     const __m128i *dm, const __m128i *cv, const __m128i *pm)
    {
        if (zx)
            vifUnpackLoop<VL, VN, true>(vuData, src, count, destVec, cl, wl, bpv, dm, cv, pm);
        else
            vifUnpackLoop<VL, VN, false>(vuData, src, count, destVec, cl, wl, bpv, dm, cv, pm);
    }

    template <int VL>
    void vifUnpackVn(uint32_t vn, bool zx, uint8_t *vuData, const uint8_t *src, uint32_t count, uint32_t destVec, uint32_t cl, uint32_t wl,
                     uint32_t bpv, const __m128i *dm, const __m128i *cv, const __m128i *pm)
    {
        switch (vn)
        {
        case 0: vifUnpackZx<VL, 0>(zx, vuData, src, count, destVec, cl, wl, bpv, dm, cv, pm); break;
        case 1: vifUnpackZx<VL, 1>(zx, vuData, src, count, destVec, cl, wl, bpv, dm, cv, pm); break;
        case 2: vifUnpackZx<VL, 2>(zx, vuData, src, count, destVec, cl, wl, bpv, dm, cv, pm); break;
        default: vifUnpackZx<VL, 3>(zx, vuData, src, count, destVec, cl, wl, bpv, dm, cv, pm); break;
        }
    }
} // namespace

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

// PERF-VS: PS2X_VIFSTAT=1 prints where the VIF1 worker's time goes (cycles inside this function, split into VU1 program runs, UNPACK and DIRECT)
namespace
{
    struct VifStat
    {
        uint64_t fn = 0, vu = 0, unpack = 0, direct = 0;
        uint64_t calls = 0, codes = 0, mscal = 0, unpacks = 0, directs = 0, directBytes = 0;
        uint64_t t0 = 0;
        std::chrono::steady_clock::time_point wall{};
    } g_vs;
    // UNPACK shape histogram: key = opcode | cl << 7 | wl << 15 | maskEnable << 23 | mode << 24 | usn << 26 | flg << 27
    struct VsShape
    {
        uint32_t key = 0;
        bool used = false;
        uint64_t n = 0, vecs = 0, ticks = 0;
    };
    VsShape g_vsShapes[256];
    VsShape *vsShapeSlot(uint32_t key)
    {
        uint32_t h = (key * 2654435761u) >> 24;
        for (uint32_t i = 0; i < 256u; ++i)
        {
            VsShape &e = g_vsShapes[(h + i) & 255u];
            if (!e.used)
            {
                e.used = true;
                e.key = key;
                return &e;
            }
            if (e.key == key)
                return &e;
        }
        return nullptr;
    }
    static const bool g_vifStatOn = []() { const char *e = std::getenv("PS2X_VIFSTAT"); return e && e[0] == '1'; }();
    inline __forceinline bool vifStatOn()
    {
        return g_vifStatOn;
    }
    struct VsTimer
    {
        uint64_t *acc;
        uint64_t t0;
        explicit VsTimer(uint64_t *a) : acc(g_vifStatOn ? a : nullptr), t0(acc ? __rdtsc() : 0) {}
        ~VsTimer()
        {
            if (acc)
                *acc += __rdtsc() - t0;
        }
    };
    struct VsShapeTimer
    {
        VsShape *slot = nullptr;
        uint64_t t0 = 0;
        void set(uint32_t key, uint32_t vecs)
        {
            if (!g_vifStatOn)
                return;
            slot = vsShapeSlot(key);
            t0 = __rdtsc();
            if (slot)
            {
                ++slot->n;
                slot->vecs += vecs;
            }
        }
        ~VsShapeTimer()
        {
            if (slot)
                slot->ticks += __rdtsc() - t0;
        }
    };
    void vifStatReport()
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_vs.wall.time_since_epoch().count() == 0)
        {
            g_vs.wall = now;
            g_vs.t0 = __rdtsc();
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(now - g_vs.wall).count();
        if (ms < 5000.0)
            return;
        const double tpms = static_cast<double>(__rdtsc() - g_vs.t0) / ms;
        const auto per = [&](uint64_t t) { return static_cast<double>(t) / tpms / ms * 1000.0; }; // ms per second
        const double fn = per(g_vs.fn), vu = per(g_vs.vu), up = per(g_vs.unpack), di = per(g_vs.direct);
        std::printf("[vifstat] per s: fn=%.0f ms (vu1 runs %.0f, unpack %.0f, direct %.0f, other parse %.0f) | calls=%.0f codes=%.0f mscal=%.0f (%.1f us each) unpack=%.0f (%.2f us each) direct=%.0f (%.1f us, %.1f KB each)\n",
                    fn, vu, up, di, fn - vu - up - di, g_vs.calls * 1000.0 / ms, g_vs.codes * 1000.0 / ms, g_vs.mscal * 1000.0 / ms,
                    g_vs.mscal ? vu * 1000.0 / (g_vs.mscal * 1000.0 / ms) : 0.0, g_vs.unpacks * 1000.0 / ms,
                    g_vs.unpacks ? up * 1000.0 / (g_vs.unpacks * 1000.0 / ms) : 0.0, g_vs.directs * 1000.0 / ms,
                    g_vs.directs ? di * 1000.0 / (g_vs.directs * 1000.0 / ms) : 0.0, g_vs.directs ? g_vs.directBytes / 1024.0 / g_vs.directs : 0.0);
        {
            // top UNPACK shapes by time: "op/cl/wl/mask/mode/usn/flg n/s vec/call ns/call ms/s"
            int order[256], cnt = 0;
            for (int i = 0; i < 256; ++i)
                if (g_vsShapes[i].used)
                    order[cnt++] = i;
            std::sort(order, order + cnt, [](int a, int b) { return g_vsShapes[a].ticks > g_vsShapes[b].ticks; });
            std::printf("[vifstat] unpack shapes (op/cl/wl/mask/mode/usn/flg: calls/s, vec/call, ns/call, ms/s):");
            for (int k = 0; k < cnt && k < 8; ++k)
            {
                const VsShape &e = g_vsShapes[order[k]];
                const double callsPerS = e.n * 1000.0 / ms;
                std::printf(" %02X/%u/%u/%u/%u/%u/%u: %.0f, %.1f, %.0f, %.1f |", e.key & 0x7Fu, (e.key >> 7) & 0xFFu, (e.key >> 15) & 0xFFu, (e.key >> 23) & 1u,
                            (e.key >> 24) & 3u, (e.key >> 26) & 1u, (e.key >> 27) & 1u, callsPerS, e.n ? static_cast<double>(e.vecs) / e.n : 0.0,
                            e.n ? static_cast<double>(e.ticks) / tpms / e.n * 1e6 : 0.0, per(e.ticks));
            }
            std::printf("\n");
            for (VsShape &e : g_vsShapes)
                e = VsShape{};
        }
        std::fflush(stdout);
        g_vs = VifStat{};
        g_vs.wall = now;
        g_vs.t0 = __rdtsc();
    }

    // Hoisted per-UNPACK / per-MSCAL flags and unpack setup cache
    static const bool s_unpackQuiet = !(probe_ae::isEnabled() || probe_ae::isDcProbeEnabled() || probe_ae::isMovieHleFrameMode() || probe_ae::isMovieHleVerify() ||
                                        isVu3dProbeEnabled());
    static const bool s_skipUnpackEnabled = probe_ae::isMovieSkipUnpackEnabled();
    static const bool s_unpackCountsOn = probe_ae::isEnabled() || probe_ae::isDcProbeEnabled() || isVu3dProbeEnabled();

    static const bool g_unpackFastEnabled = []() -> bool {
        const char *env = std::getenv("PS2X_UNPACK_FAST");
        return !(env && (env[0] == '0' || env[0] == 'f' || env[0] == 'F'));
    }();

    static const bool g_unpackVerifyEnabled = []() -> bool {
        const char *env = std::getenv("PS2X_UNPACK_VERIFY");
        return env && (env[0] == '1' || env[0] == 't' || env[0] == 'T');
    }();

    inline __forceinline bool isUnpackFastEnabled() { return g_unpackFastEnabled; }
    inline __forceinline bool isUnpackVerifyEnabled() { return g_unpackVerifyEnabled; }

    static const bool s_uvOn = []() {
        const char *e = std::getenv("PS2X_UNPACK_SIMD");
        return !(e && e[0] == '0');
    }(); // PERF-UV

    enum Act : uint8_t {
        ACT_DECODED = 0,
        ACT_CONST   = 1,
        ACT_PROTECT = 2
    };

    // Task MG1: VIF1 two-stage pipeline (parser || VU1 exec), file-static engine
    enum VpEventKind : uint8_t {
        VP_WRITES = 1,
        VP_MSCAL  = 2,
        VP_MSCNT  = 3,
        VP_GIF2   = 4,
        VP_FLUSH  = 5
    };

    struct VpEvent {
        uint8_t kind;
        uint8_t hl;
        uint32_t a, b, c;
        const uint8_t *p;
        uint32_t n;
        uint32_t t;
    };

    struct VpRec {
        uint16_t qw;
        uint8_t mask;
        uint8_t pad;
        uint32_t lanes[4];
    };

    static constexpr uint32_t kVpEventCap = 65536;
    static VpEvent vpEventRing[kVpEventCap];
    static std::atomic<uint32_t> vpEventHead{0};
    static std::atomic<uint32_t> vpEventTail{0};
    static std::atomic<bool> vpConsumerWaiting{false};
    static std::mutex vpConsumerMutex;
    static std::condition_variable vpConsumerCv;
    static std::atomic<bool> vpProducerWaiting{false};
    static std::mutex vpProducerMutex;
    static std::condition_variable vpProducerCv;

    static constexpr uint32_t kVpRecCap = 262144; // smaller rings (16K/64K) filled constantly and the full-ring spin cost more than it saved (A/B mh3)
    static VpRec vpRecRing[kVpRecCap];
    static std::atomic<uint32_t> vpRecHead{0};
    static std::atomic<uint32_t> vpRecTail{0};
    static std::atomic<bool> vpRecProducerWaiting{false};
    static std::mutex vpRecProducerMutex;
    static std::condition_variable vpRecProducerCv;

    // PERF-V1H: coalesced pending VP_WRITES run (parser thread only)
    static constexpr uint32_t kVpWriteCoalesceCap = 4096u;
    static uint32_t s_vpPendingFirst = 0u;
    static uint32_t s_vpPendingCount = 0u;
    static inline void vpFlushPendingWrites();

    alignas(16) static uint8_t s_vpScratch[PS2_VU1_DATA_SIZE];
    static uint8_t *s_vpVu1Data = nullptr;

    static std::function<void(uint32_t, uint32_t, uint32_t)> vpMscalCb;
    static void (*vpMscalFn)(void *, uint32_t, uint32_t, uint32_t) = nullptr; // PERF-V1G
    static void *vpMscalCtx = nullptr; // PERF-V1G
    static std::function<void(uint32_t, uint32_t)> vpMscntCb;
    static std::function<void(const uint8_t *, uint32_t, bool)> vpGif2Cb;
    static std::function<void(const uint8_t *, uint32_t)> vpGif1Cb;

    extern "C" void ps2_vu1_set_capture(std::vector<uint8_t> *v);
} // namespace (DB1: global-linkage extern must sit outside the anonymous namespace)
extern std::function<void(uint32_t startPC, uint32_t top, uint32_t itop, uint8_t *vuData)> g_vu1MscalCallbackB;
extern "C" void ps2_vu1_set_kick_record(std::vector<uint32_t> *v);
namespace
{

    static inline bool vpVerifyOn()
    {
        static const bool on = []() {
            const char *e = std::getenv("PS2X_VU1_DUAL_VERIFY");
            return e && e[0] == '1';
        }();
        return on;
    }

    static inline bool vpDualOn()
    {
        static const bool on = []() {
            const char *e = std::getenv("PS2X_VU1_DUAL");
            return (e && e[0] == '1') || vpVerifyOn();
        }();
        return on;
    }

    static std::atomic<uint32_t> vpEventHead1{0};
    static std::atomic<bool> vpConsumer1Waiting{false};
    static std::mutex vpConsumer1Mutex;
    static std::condition_variable vpConsumer1Cv;

    static std::atomic<uint32_t> vpRecHead1{0};

    alignas(64) static uint8_t s_vpShadow[PS2_VU1_DATA_SIZE];

    static thread_local std::bitset<1024> s_tlVifWrites;
    static thread_local std::bitset<1024> s_tlVifAfterPrev;

    // Task DB2/DB3: Dual VU1 executor state
    struct VpSlot {
        std::atomic<uint32_t> done{0}; // k+1 when published, 0 after reset
        uint8_t w[161][16];            // kept for VERIFY mode
        bool swapped{false};
        bool kickOnly{false};
        std::vector<uint16_t> dq;
        std::vector<std::array<uint8_t, 16>> dv;
        std::vector<uint8_t> gif;      // kept for VERIFY mode
        std::vector<uint8_t> entryMem; // kept for VERIFY mode
        std::atomic<bool> entryReady{false}; // kept for VERIFY mode
    };
    static VpSlot s_vpSlots[256];
    static std::atomic<uint32_t> vpVerifyDone{0};

    struct PredEntry {
        uint8_t key[16];
        uint8_t state; // 0 empty, 1 known, 2 conflict
        uint8_t swaps;
        uint8_t kickOnly;
        uint16_t kYes, kNo, sYes, sNo; // PERF-DB3P saturating outcome counters
    };
    static PredEntry s_predTable[1024];
    static std::mutex s_predMutex;

    static uint32_t s_vpMscalK = 0u; // Parser only: MSCAL event index within job
    static uint32_t s_vpTurnT = 0u;   // Parser only: turn index over MSCAL and GIF2
    static std::atomic<uint32_t> s_vpTurnDone{0}; // Shared: completed turn counter

    static std::atomic<uint64_t> s_vpStX1Runs{0};
    static std::atomic<uint64_t> s_vpStSpec{0};
    static std::atomic<uint64_t> s_vpStReruns{0};
    static std::atomic<uint64_t> s_vpStWaitsExact{0};
    static std::atomic<uint64_t> s_vpStApplyQw{0};
    static std::atomic<uint64_t> s_vpStDiffQw{0};
    alignas(16) static thread_local uint8_t s_threadKeys[256][16];

    static inline uint32_t hashKey16(const uint8_t *k)
    {
        uint64_t a, b;
        std::memcpy(&a, k, 8);
        std::memcpy(&b, k + 8, 8);
        uint64_t h = a ^ (b * 0x517cc1b727220a95ULL);
        h ^= (h >> 32);
        return static_cast<uint32_t>(h);
    }

    static PredEntry predLookup(const uint8_t *key)
    {
        std::lock_guard<std::mutex> lock(s_predMutex);
        const uint32_t idx = hashKey16(key) & 1023u;
        for (uint32_t step = 0u; step < 1024u; ++step)
        {
            const uint32_t cur = (idx + step) & 1023u;
            if (s_predTable[cur].state == 0u)
                break;
            if (std::memcmp(s_predTable[cur].key, key, 16) == 0)
                return s_predTable[cur];
        }
        PredEntry empty{};
        empty.state = 0u;
        return empty;
    }

    static bool vpStatOn(); // PROBE-DB3H (forward decl; defined below)
    static std::atomic<uint32_t> s_db3NonKickHist[1024]; // PROBE-DB3H
    static std::atomic<uint64_t> s_db3NonKickProgs{0};   // PROBE-DB3H

    static void predUpdate(const uint8_t *key, bool swapped, bool kickOnly)
    {
        std::lock_guard<std::mutex> lock(s_predMutex);
        const uint32_t idx = hashKey16(key) & 1023u;
        uint32_t emptyIdx = 1024u;
        for (uint32_t step = 0u; step < 1024u; ++step)
        {
            const uint32_t cur = (idx + step) & 1023u;
            if (s_predTable[cur].state == 0u)
            {
                if (emptyIdx == 1024u)
                    emptyIdx = cur;
                break;
            }
            if (std::memcmp(s_predTable[cur].key, key, 16) == 0)
            {
                { // PERF-DB3P majority-based prediction (never state=2)
                    PredEntry &e = s_predTable[cur];
                    if (swapped) ++e.sYes; else ++e.sNo;
                    if (kickOnly) ++e.kYes; else ++e.kNo;
                    if (e.sYes >= 60000u || e.sNo >= 60000u || e.kYes >= 60000u || e.kNo >= 60000u)
                    {
                        e.sYes >>= 1; e.sNo >>= 1; e.kYes >>= 1; e.kNo >>= 1;
                    }
                    e.swaps = (e.sYes >= e.sNo) ? 1u : 0u;
                    e.kickOnly = ((uint32_t)e.kYes >= 3u * (uint32_t)e.kNo + 1u) ? 1u : 0u;
                }
                return;
            }
        }
        if (emptyIdx < 1024u)
        {
            std::memcpy(s_predTable[emptyIdx].key, key, 16);
            s_predTable[emptyIdx].state = 1u;
            s_predTable[emptyIdx].swaps = swapped ? 1u : 0u;
            s_predTable[emptyIdx].kickOnly = kickOnly ? 1u : 0u;
            s_predTable[emptyIdx].sYes = swapped ? 1u : 0u;   // PERF-DB3P
            s_predTable[emptyIdx].sNo = swapped ? 0u : 1u;
            s_predTable[emptyIdx].kYes = kickOnly ? 1u : 0u;
            s_predTable[emptyIdx].kNo = kickOnly ? 0u : 1u;
        }
    }

    static inline void loadW(uint8_t dst[161][16], const uint8_t *vuData)
    {
        std::memcpy(dst[0], vuData + (1023u * 16u), 16);
        std::memcpy(&dst[1][0], vuData + (636u * 16u), 160u * 16u);
    }

    static inline void storeW(uint8_t *vuData, const uint8_t src[161][16])
    {
        std::memcpy(vuData + (1023u * 16u), src[0], 16);
        std::memcpy(vuData + (636u * 16u), &src[1][0], 160u * 16u);
    }

    static inline void swapToggle1023(uint8_t *vuData)
    {
        uint32_t *qw = reinterpret_cast<uint32_t *>(vuData + (1023u * 16u));
        std::swap(qw[2], qw[3]);
    }

    static std::atomic<uint32_t> vpFlushSeqGen{0};
    static std::atomic<uint32_t> vpIdleSeq{0};
    static std::atomic<uint32_t> vpIdleSeq1{0};
    static std::mutex vpFlushMutex;
    static std::condition_variable vpFlushCv;

    static std::atomic<uint64_t> s_vpStBusy1{0};

    static void vpExec2ThreadFunc();
    static std::once_flag vpThread1Once;
    static std::atomic<bool> vpThread1Started{false};
    static void vpEnsureThread1Started()
    {
        std::call_once(vpThread1Once, []() {
            vpThread1Started.store(true, std::memory_order_seq_cst);
            std::thread(vpExec2ThreadFunc).detach();
        });
    }

    static inline uint32_t vpEventLag(uint32_t tail, bool dual)
    {
        const uint32_t h0 = vpEventHead.load(std::memory_order_seq_cst);
        uint32_t lag = tail - h0;
        if (dual && vpThread1Started.load(std::memory_order_seq_cst))
        {
            const uint32_t h1 = vpEventHead1.load(std::memory_order_seq_cst);
            const uint32_t lag1 = tail - h1;
            if (lag1 > lag)
                lag = lag1;
        }
        return lag;
    }

    static inline uint32_t vpRecLag(uint32_t tail, bool dual)
    {
        const uint32_t h0 = vpRecHead.load(std::memory_order_seq_cst);
        uint32_t lag = tail - h0;
        if (dual && vpThread1Started.load(std::memory_order_seq_cst))
        {
            const uint32_t h1 = vpRecHead1.load(std::memory_order_seq_cst);
            const uint32_t lag1 = tail - h1;
            if (lag1 > lag)
                lag = lag1;
        }
        return lag;
    }

    static void vpExecThreadFunc();
    static std::once_flag vpThreadOnce;
    static void vpEnsureThreadStarted()
    {
        std::call_once(vpThreadOnce, []() {
            std::thread(vpExecThreadFunc).detach();
        });
    }

    static int vpSpinLimit() // PERF-MG2: PS2X_VPIPE_SPIN = pause iterations before a ring wait sleeps (default 4000)
    {
        static const int n = []() { const char *e = std::getenv("PS2X_VPIPE_SPIN"); return e ? static_cast<int>(std::strtol(e, nullptr, 10)) : 4000; }();
        return n;
    }

    template <typename Pred>
    static inline void vpWaitCond(Pred &&pred)
    {
        if (pred())
            return;
        const int spinLimit = vpSpinLimit();
        for (int spin = 0; spin < spinLimit; ++spin)
        {
            if (pred())
                return;
            _mm_pause();
        }
        while (!pred())
        {
            std::this_thread::yield();
        }
    }

    static void vpRunMscal(const VpEvent &ev, int myThread)
    {
        const uint32_t k = ev.n;
        uint8_t *copy = (myThread == 0) ? s_vpVu1Data : s_vpShadow;
        if (!copy)
            return;

        // Section 3: On every VP_MSCAL event (own or not), save V into Vprev and clear V
        s_tlVifAfterPrev = s_tlVifWrites;
        s_tlVifWrites.reset();

        if ((k & 1u) != static_cast<uint32_t>(myThread))
        {
            // If k is NOT this thread's: read key(k) = 16 bytes at own copy + top*16 and keep it; continue.
            std::memcpy(s_threadKeys[k & 255u], copy + (static_cast<size_t>(ev.b & 0x3FFu) * 16u), 16);
            return;
        }

        // If it is this thread's:
        if (myThread == 1)
            s_vpStX1Runs.fetch_add(1, std::memory_order_relaxed);

        // Read and save key(k) before running (microcode could overwrite TOP)
        std::memcpy(s_threadKeys[k & 255u], copy + (static_cast<size_t>(ev.b & 0x3FFu) * 16u), 16);

        alignas(64) static thread_local uint8_t E[PS2_VU1_DATA_SIZE];
        static thread_local std::vector<uint8_t> s_cap;
        static thread_local std::vector<uint32_t> s_kickRec;

        auto runProgram = [&]() {
            s_cap.clear();
            s_kickRec.clear();
            ps2_vu1_set_capture(&s_cap);
            ps2_vu1_set_kick_record(&s_kickRec);
            if (myThread == 0)
            {
                if (vpMscalCb)
                    vpMscalCb(ev.a, ev.b, ev.c);
            }
            else
            {
                if (g_vu1MscalCallbackB)
                    g_vu1MscalCallbackB(ev.a, ev.b, ev.c, s_vpShadow);
            }
            ps2_vu1_set_capture(nullptr);
            ps2_vu1_set_kick_record(nullptr);
        };

        auto applyPrev = [&](uint32_t prevK) {
            const auto &slotPrev = s_vpSlots[prevK & 255u];
            for (size_t i = 0; i < slotPrev.dq.size(); ++i)
            {
                const uint16_t q = slotPrev.dq[i];
                if (!s_tlVifAfterPrev.test(q))
                {
                    std::memcpy(copy + (q * 16u), slotPrev.dv[i].data(), 16);
                    s_vpStApplyQw.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        auto computeDiff = [&](uint32_t curK, std::bitset<1024> &outDiffBits, bool &outKickOnly, bool &outSwapped) {
            auto &slot = s_vpSlots[curK & 255u];
            if (slot.dq.capacity() < 1024)
            {
                slot.dq.reserve(1024);
                slot.dv.reserve(1024);
            }
            slot.dq.clear();
            slot.dv.clear();
            outDiffBits.reset();
            for (uint32_t q = 0u; q < 1024u; ++q)
            {
                if (std::memcmp(copy + (q * 16u), E + (q * 16u), 16) != 0)
                {
                    outDiffBits.set(q);
                    slot.dq.push_back(static_cast<uint16_t>(q));
                    std::array<uint8_t, 16> val;
                    std::memcpy(val.data(), copy + (q * 16u), 16);
                    slot.dv.push_back(val);
                }
            }
            s_vpStDiffQw.fetch_add(slot.dq.size(), std::memory_order_relaxed);
            outSwapped = outDiffBits.test(1023);
            outKickOnly = true;
            uint32_t db3Non = 0; // PROBE-DB3H
            for (uint16_t q : slot.dq)
            {
                if (q == 1023u)
                    continue;
                bool inKick = false;
                for (size_t i = 0; i + 1 < s_kickRec.size(); i += 2)
                {
                    const uint32_t start = s_kickRec[i] & 1023u;
                    const uint32_t count = s_kickRec[i + 1];
                    if (count >= 1024u || ((q + 1024u - start) & 1023u) < count)
                    {
                        inKick = true;
                        break;
                    }
                }
                if (!inKick)
                {
                    outKickOnly = false;
                    if (vpStatOn()) { s_db3NonKickHist[q].fetch_add(1, std::memory_order_relaxed); ++db3Non; } // PROBE-DB3H (no break: count all)
                    else break;
                }
            }
            if (db3Non) s_db3NonKickProgs.fetch_add(1, std::memory_order_relaxed); // PROBE-DB3H
            predUpdate(s_threadKeys[curK & 255u], outSwapped, outKickOnly);
            slot.swapped = outSwapped;
            slot.kickOnly = outKickOnly;
        };

        if (k == 0u)
        {
            std::memcpy(E, copy, PS2_VU1_DATA_SIZE);
            runProgram();
            std::bitset<1024> kDiffBits;
            bool kickOnly = false, swapped = false;
            computeDiff(0u, kDiffBits, kickOnly, swapped);
            s_vpSlots[0].done.store(1u, std::memory_order_release);
        }
        else
        {
            const bool prevDone = (s_vpSlots[(k - 1u) & 255u].done.load(std::memory_order_acquire) == k);
            bool speculate = false;
            PredEntry pred{};
            if (!prevDone)
            {
                pred = predLookup(s_threadKeys[(k - 1u) & 255u]);
                if (pred.state == 1u && pred.kickOnly != 0u)
                    speculate = true;
            }

            if (speculate)
            {
                s_vpStSpec.fetch_add(1, std::memory_order_relaxed);
                const bool predictedSwap = (pred.swaps != 0u);
                if (predictedSwap)
                    swapToggle1023(copy);
                std::memcpy(E, copy, PS2_VU1_DATA_SIZE);

                runProgram();

                vpWaitCond([&]() {
                    return s_vpSlots[(k - 1u) & 255u].done.load(std::memory_order_acquire) == k;
                });
                const auto &slotPrev = s_vpSlots[(k - 1u) & 255u];
                const bool valid = slotPrev.kickOnly && (slotPrev.swapped == predictedSwap);

                if (valid)
                {
                    std::bitset<1024> kDiffBits;
                    bool kickOnly = false, swapped = false;
                    computeDiff(k, kDiffBits, kickOnly, swapped);

                    // Apply k-1 skipping every q that program k wrote and skipping 1023
                    for (size_t i = 0; i < slotPrev.dq.size(); ++i)
                    {
                        const uint16_t q = slotPrev.dq[i];
                        if (q == 1023u)
                            continue;
                        if (kDiffBits.test(q))
                            continue;
                        if (!s_tlVifAfterPrev.test(q))
                        {
                            std::memcpy(copy + (q * 16u), slotPrev.dv[i].data(), 16);
                            s_vpStApplyQw.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    s_vpSlots[k & 255u].done.store(k + 1u, std::memory_order_release);
                }
                else
                {
                    s_vpStReruns.fetch_add(1, std::memory_order_relaxed);
                    std::memcpy(copy, E, PS2_VU1_DATA_SIZE);
                    if (predictedSwap)
                        swapToggle1023(copy);

                    applyPrev(k - 1u);

                    std::memcpy(E, copy, PS2_VU1_DATA_SIZE);
                    runProgram();

                    std::bitset<1024> kDiffBits;
                    bool kickOnly = false, swapped = false;
                    computeDiff(k, kDiffBits, kickOnly, swapped);
                    s_vpSlots[k & 255u].done.store(k + 1u, std::memory_order_release);
                }
            }
            else
            {
                if (!prevDone)
                {
                    s_vpStWaitsExact.fetch_add(1, std::memory_order_relaxed);
                    vpWaitCond([&]() {
                        return s_vpSlots[(k - 1u) & 255u].done.load(std::memory_order_acquire) == k;
                    });
                }
                applyPrev(k - 1u);

                std::memcpy(E, copy, PS2_VU1_DATA_SIZE);
                runProgram();

                std::bitset<1024> kDiffBits;
                bool kickOnly = false, swapped = false;
                computeDiff(k, kDiffBits, kickOnly, swapped);
                s_vpSlots[k & 255u].done.store(k + 1u, std::memory_order_release);
            }
        }

        // 5. Wait for the GIF turn t (atomic turnDone == t), submit the capture with submitGifPacket(Path1, ...) (X1 uses the same callback object; submitGifPacket is called by whichever thread holds the turn), then ps2_gs_async_batch(false); ps2_gs_async_batch(true) (flush so cross-thread order holds), then turnDone.store(t+1).
        vpWaitCond([&]() {
            return s_vpTurnDone.load(std::memory_order_acquire) == ev.t;
        });
        if (!s_cap.empty() && vpGif1Cb)
            vpGif1Cb(s_cap.data(), static_cast<uint32_t>(s_cap.size()));
        ps2_gs_async_batch(false);
        ps2_gs_async_batch(true);
        s_vpTurnDone.store(ev.t + 1u, std::memory_order_release);
    }

    // Task DC: VU1 dual executor VERIFY mode
    static std::atomic<uint64_t> s_vvCompared{0};
    static std::atomic<uint64_t> s_vvGifMismatch{0};
    static std::atomic<uint64_t> s_vvWMismatch{0};
    static std::atomic<uint64_t> s_vvEntryDiffPrograms{0};
    static std::atomic<uint64_t> s_vvEntryDiffQwords{0};
    static std::atomic<uint32_t> s_vvMismatchPrints{0};
    static std::atomic<uint32_t> s_vvEntryPrints{0};
    static uint32_t s_vvHist[1024]{};

    static inline void vpVerifySummaryPrint()
    {
        static std::atomic<bool> s_printing{false};
        static auto s_lastSummary = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastSummary).count() >= 1)
        {
            bool expected = false;
            if (s_printing.compare_exchange_strong(expected, true))
            {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastSummary).count() >= 1)
                {
                    s_lastSummary = now;
                    ::ps2xSafeLog("[vverify] compared=%llu gifMismatch=%llu wMismatch=%llu entryDiffPrograms=%llu entryDiffQwords=%llu\n",
                                  (unsigned long long)s_vvCompared.exchange(0),
                                  (unsigned long long)s_vvGifMismatch.exchange(0),
                                  (unsigned long long)s_vvWMismatch.exchange(0),
                                  (unsigned long long)s_vvEntryDiffPrograms.exchange(0),
                                  (unsigned long long)s_vvEntryDiffQwords.exchange(0));

                    struct HistEntry {
                        uint32_t idx;
                        uint32_t count;
                    };
                    HistEntry sortedHist[1024];
                    for (uint32_t i = 0; i < 1024; ++i)
                    {
                        sortedHist[i].idx = i;
                        sortedHist[i].count = s_vvHist[i];
                    }
                    std::partial_sort(sortedHist, sortedHist + 24, sortedHist + 1024,
                                      [](const HistEntry &a, const HistEntry &b) {
                                          if (a.count != b.count)
                                              return a.count > b.count;
                                          return a.idx < b.idx;
                                      });

                    char hotBuf[512];
                    int pos = std::snprintf(hotBuf, sizeof(hotBuf), "[vverify] hot q:");
                    for (int i = 0; i < 24; ++i)
                    {
                        if (sortedHist[i].count == 0)
                            break;
                        pos += std::snprintf(hotBuf + pos, sizeof(hotBuf) - pos, " %u:%u", sortedHist[i].idx, sortedHist[i].count);
                    }
                    std::snprintf(hotBuf + pos, sizeof(hotBuf) - pos, "\n");
                    ::ps2xSafeLog("%s", hotBuf);
                }
                s_printing.store(false, std::memory_order_relaxed);
            }
        }
    }

    static void vpRunVerifyX0(const VpEvent &ev)
    {
        const uint32_t k = ev.n;
        uint8_t *copy = s_vpVu1Data;
        if (!copy)
            return;

        uint8_t entryW[161][16];
        loadW(entryW, copy);
        std::memcpy(s_threadKeys[k & 255u], copy + (static_cast<size_t>(ev.b & 0x3FFu) * 16u), 16);

        if ((k & 1u) != 0u)
        {
            auto &slot = s_vpSlots[k & 255u];
            if (slot.entryMem.size() != PS2_VU1_DATA_SIZE)
                slot.entryMem.resize(PS2_VU1_DATA_SIZE);
            std::memcpy(slot.entryMem.data(), copy, PS2_VU1_DATA_SIZE);
            slot.entryReady.store(true, std::memory_order_release);
        }

        static thread_local std::vector<uint8_t> s_cap0;
        s_cap0.clear();
        ps2_vu1_set_capture(&s_cap0);
        if (vpMscalCb)
            vpMscalCb(ev.a, ev.b, ev.c);
        ps2_vu1_set_capture(nullptr);

        if (!s_cap0.empty() && vpGif1Cb)
            vpGif1Cb(s_cap0.data(), static_cast<uint32_t>(s_cap0.size()));

        vpWaitCond([&]() {
            return k < vpVerifyDone.load(std::memory_order_acquire) + 200u;
        });

        loadW(s_vpSlots[k & 255u].w, copy);
        s_vpSlots[k & 255u].gif = s_cap0;
        s_vpSlots[k & 255u].done.store(k + 1u, std::memory_order_release);
    }

    static void vpRunVerifyX1(const VpEvent &ev)
    {
        const uint32_t k = ev.n;
        uint8_t *copy = s_vpShadow;
        if (!copy)
            return;

        if ((k & 1u) == 0u)
        {
            std::memcpy(s_threadKeys[k & 255u], copy + (static_cast<size_t>(ev.b & 0x3FFu) * 16u), 16);
            return;
        }

        s_vpStX1Runs.fetch_add(1, std::memory_order_relaxed);
        std::memcpy(s_threadKeys[k & 255u], copy + (static_cast<size_t>(ev.b & 0x3FFu) * 16u), 16);

        if (k != 0u)
        {
            vpWaitCond([&]() {
                return s_vpSlots[(k - 1u) & 255u].done.load(std::memory_order_acquire) == k;
            });
            storeW(copy, s_vpSlots[(k - 1u) & 255u].w);
        }

        vpWaitCond([&]() {
            return s_vpSlots[k & 255u].done.load(std::memory_order_acquire) == k + 1u;
        });

        const auto &entryMem = s_vpSlots[k & 255u].entryMem;
        uint32_t diffQwords = 0u;
        struct QRange {
            uint32_t start;
            uint32_t end;
        };
        QRange ranges[8];
        uint32_t rangeCount = 0u;
        bool inRange = false;
        bool activeRangeSaved = false;

        if (entryMem.size() == PS2_VU1_DATA_SIZE)
        {
            for (uint32_t q = 0u; q < 1024u; ++q)
            {
                if (std::memcmp(copy + (q * 16u), entryMem.data() + (q * 16u), 16) != 0)
                {
                    diffQwords++;
                    s_vvHist[q]++;
                    if (!inRange)
                    {
                        inRange = true;
                        if (rangeCount < 8u)
                        {
                            ranges[rangeCount].start = q;
                            ranges[rangeCount].end = q;
                            rangeCount++;
                            activeRangeSaved = true;
                        }
                        else
                        {
                            activeRangeSaved = false;
                        }
                    }
                    else if (activeRangeSaved)
                    {
                        ranges[rangeCount - 1u].end = q;
                    }
                }
                else
                {
                    inRange = false;
                    activeRangeSaved = false;
                }
            }
        }

        { // PROBE-DC3: PS2X_VU1_DUAL_VERIFY_SYNC=1 runs X1 on X0's exact entry memory (checks the second VU1 instance itself)
            static const bool s_dc3 = []() { const char *e = std::getenv("PS2X_VU1_DUAL_VERIFY_SYNC"); return e && e[0] == '1'; }();
            if (s_dc3 && entryMem.size() == PS2_VU1_DATA_SIZE)
                std::memcpy(copy, entryMem.data(), PS2_VU1_DATA_SIZE);
        }
        static thread_local std::vector<uint8_t> s_cap1;
        s_cap1.clear();
        ps2_vu1_set_capture(&s_cap1);
        if (g_vu1MscalCallbackB)
            g_vu1MscalCallbackB(ev.a, ev.b, ev.c, s_vpShadow);
        ps2_vu1_set_capture(nullptr);

        const auto &x0Gif = s_vpSlots[k & 255u].gif;
        const auto &x1Gif = s_cap1;
        int firstDiff = -1;
        const size_t minSz = std::min(x0Gif.size(), x1Gif.size());
        for (size_t i = 0; i < minSz; ++i)
        {
            if (x0Gif[i] != x1Gif[i])
            {
                firstDiff = static_cast<int>(i);
                break;
            }
        }
        if (firstDiff == -1 && x0Gif.size() != x1Gif.size())
        {
            firstDiff = static_cast<int>(minSz);
        }
        const bool gifMismatch = (firstDiff != -1);

        uint8_t shadowW[161][16];
        loadW(shadowW, copy);
        char wBuf[32];
        int diffQword = -1;
        if (std::memcmp(shadowW[0], s_vpSlots[k & 255u].w[0], 16) != 0)
        {
            diffQword = 1023;
        }
        else
        {
            for (uint32_t i = 1u; i < 161u; ++i)
            {
                if (std::memcmp(shadowW[i], s_vpSlots[k & 255u].w[i], 16) != 0)
                {
                    diffQword = static_cast<int>(635u + i);
                    break;
                }
            }
        }
        const bool wMismatch = (diffQword != -1);
        if (wMismatch)
        {
            std::snprintf(wBuf, sizeof(wBuf), "diff q=%d", diffQword);
        }
        else
        {
            std::snprintf(wBuf, sizeof(wBuf), "same");
        }

        s_vvCompared.fetch_add(1, std::memory_order_relaxed);
        if (gifMismatch)
            s_vvGifMismatch.fetch_add(1, std::memory_order_relaxed);
        if (wMismatch)
            s_vvWMismatch.fetch_add(1, std::memory_order_relaxed);
        if (diffQwords > 0u)
            s_vvEntryDiffPrograms.fetch_add(1, std::memory_order_relaxed);
        s_vvEntryDiffQwords.fetch_add(diffQwords, std::memory_order_relaxed);

        if (diffQwords > 0u)
        {
            const uint32_t entryPrintIdx = s_vvEntryPrints.fetch_add(1, std::memory_order_relaxed);
            if (entryPrintIdx < 10u)
            {
                char rangesBuf[128];
                int rpos = 0;
                rangesBuf[0] = '\0';
                for (uint32_t r = 0u; r < rangeCount; ++r)
                {
                    rpos += std::snprintf(rangesBuf + rpos, sizeof(rangesBuf) - rpos,
                                          (r == 0u) ? "%u-%u" : ",%u-%u",
                                          ranges[r].start, ranges[r].end);
                }
                ::ps2xSafeLog("[vverify] entry k=%u pc=%04x top=%u n=%u ranges=%s gif=%s\n",
                              k, ev.a, ev.b, diffQwords, rangesBuf, gifMismatch ? "diff" : "same");
            }
        }

        if (gifMismatch || wMismatch)
        {
            const uint32_t printIdx = s_vvMismatchPrints.fetch_add(1, std::memory_order_relaxed);
            if (printIdx < 20u)
            {
                ::ps2xSafeLog("[vverify] k=%u pc=%04x top=%u itop=%u gif x0=%u x1=%u firstDiff=%d w=%s\n",
                              k, ev.a, ev.b, ev.c,
                              static_cast<uint32_t>(x0Gif.size()), static_cast<uint32_t>(x1Gif.size()),
                              firstDiff, wBuf);
            }
        }

        vpVerifySummaryPrint();

        vpVerifyDone.store(k + 1u, std::memory_order_release);
    }

    static void vpPushEvent(const VpEvent &ev)
    {
        if (ev.kind != VP_WRITES) // PERF-V1H
            vpFlushPendingWrites();
        const bool dual = vpDualOn();
        const uint32_t tail = vpEventTail.load(std::memory_order_seq_cst);
        if (vpEventLag(tail, dual) >= kVpEventCap)
        {
            const uint64_t vwT = vw::on() ? __rdtsc() : 0ull; // PROBE-VW
            for (int spin = 0; spin < vpSpinLimit(); ++spin)
            {
                if (vpEventLag(tail, dual) < kVpEventCap)
                    break;
                _mm_pause();
            }
            if (vpEventLag(tail, dual) >= kVpEventCap)
            {
                std::unique_lock<std::mutex> lock(vpProducerMutex);
                vpProducerWaiting.store(true, std::memory_order_seq_cst);
                vpProducerCv.wait(lock, [&]() {
                    return vpEventLag(tail, dual) < kVpEventCap;
                });
                vpProducerWaiting.store(false, std::memory_order_seq_cst);
            }
            if (vwT) vwBlk(false, vwT); // PROBE-VW
        }
        vpEventRing[tail & (kVpEventCap - 1)] = ev;
        vpEventTail.store(tail + 1, std::memory_order_seq_cst);
        if (vpConsumerWaiting.load(std::memory_order_seq_cst))
        {
            std::lock_guard<std::mutex> lock(vpConsumerMutex);
            vpConsumerCv.notify_one();
        }
        if (dual && vpConsumer1Waiting.load(std::memory_order_seq_cst))
        {
            std::lock_guard<std::mutex> lock(vpConsumer1Mutex);
            vpConsumer1Cv.notify_one();
        }
    }

    // PERF-V1H: flush coalesced VP_WRITES run before any other event, wait/drain, cap, or job end
    static inline void vpFlushPendingWrites()
    {
        if (s_vpPendingCount == 0u)
            return;
        VpEvent ev{};
        ev.kind = VP_WRITES;
        ev.a = s_vpPendingFirst;
        ev.b = s_vpPendingCount;
        s_vpPendingCount = 0u;
        vpPushEvent(ev);
    }

    // PERF-V1H: coalesce contiguous UNPACK record runs into one VP_WRITES event (disabled in dual/verify mode)
    static inline void vpQueueWrites(uint32_t firstRecIndex, uint32_t recCount)
    {
        if (recCount == 0u)
            return;
        if (vpDualOn())
        {
            VpEvent ev{};
            ev.kind = VP_WRITES;
            ev.a = firstRecIndex;
            ev.b = recCount;
            vpPushEvent(ev);
            return;
        }
        if (s_vpPendingCount != 0u &&
            s_vpPendingFirst + s_vpPendingCount == firstRecIndex &&
            s_vpPendingCount + recCount <= kVpWriteCoalesceCap)
        {
            s_vpPendingCount += recCount;
            if (s_vpPendingCount >= kVpWriteCoalesceCap)
                vpFlushPendingWrites();
            return;
        }
        vpFlushPendingWrites();
        s_vpPendingFirst = firstRecIndex;
        s_vpPendingCount = recCount;
        if (s_vpPendingCount >= kVpWriteCoalesceCap)
            vpFlushPendingWrites();
    }

    static VpEvent vpPopEvent1()
    {
        const uint32_t head = vpEventHead1.load(std::memory_order_seq_cst);
        if (head == vpEventTail.load(std::memory_order_seq_cst))
        {
            for (int spin = 0; spin < vpSpinLimit(); ++spin)
            {
                if (head != vpEventTail.load(std::memory_order_seq_cst))
                    break;
                _mm_pause();
            }
            if (head == vpEventTail.load(std::memory_order_seq_cst))
            {
                std::unique_lock<std::mutex> lock(vpConsumer1Mutex);
                vpConsumer1Waiting.store(true, std::memory_order_seq_cst);
                vpConsumer1Cv.wait(lock, [&]() {
                    return head != vpEventTail.load(std::memory_order_seq_cst);
                });
                vpConsumer1Waiting.store(false, std::memory_order_seq_cst);
            }
        }
        VpEvent ev = vpEventRing[head & (kVpEventCap - 1)];
        vpEventHead1.store(head + 1, std::memory_order_seq_cst);
        if (vpProducerWaiting.load(std::memory_order_seq_cst))
        {
            std::lock_guard<std::mutex> lock(vpProducerMutex);
            vpProducerCv.notify_one();
        }
        return ev;
    }

    // PROBE-MG: pipeline stats (PS2X_VIF1_PIPE_STAT=1): exec busy/spin ticks, sleeps, drains, producer full-waits
    static std::atomic<uint64_t> s_vpStBusy{0}, s_vpStSpin{0}, s_vpStSleeps{0}, s_vpStDrains{0}, s_vpStDrainWait{0}, s_vpStFull{0}, s_vpStEv[8];
    static bool vpStatOn()
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_VIF1_PIPE_STAT"); return e && e[0] == '1'; }();
        return on;
    }
    static VpEvent vpPopEvent()
    {
        const uint32_t head = vpEventHead.load(std::memory_order_seq_cst);
        if (head == vpEventTail.load(std::memory_order_seq_cst))
        {
            const uint64_t spT0 = vpStatOn() ? __rdtsc() : 0ull; // PROBE-MG
            struct SpinAcct { uint64_t t0; ~SpinAcct() { if (t0) s_vpStSpin.fetch_add(__rdtsc() - t0, std::memory_order_relaxed); } } spinAcct{spT0};
            for (int spin = 0; spin < vpSpinLimit(); ++spin)
            {
                if (head != vpEventTail.load(std::memory_order_seq_cst))
                    break;
                _mm_pause();
            }
            if (head == vpEventTail.load(std::memory_order_seq_cst))
            {
                if (vpStatOn()) s_vpStSleeps.fetch_add(1, std::memory_order_relaxed); // PROBE-MG
                std::unique_lock<std::mutex> lock(vpConsumerMutex);
                vpConsumerWaiting.store(true, std::memory_order_seq_cst);
                vpConsumerCv.wait(lock, [&]() {
                    return head != vpEventTail.load(std::memory_order_seq_cst);
                });
                vpConsumerWaiting.store(false, std::memory_order_seq_cst);
            }
        }
        VpEvent ev = vpEventRing[head & (kVpEventCap - 1)];
        vpEventHead.store(head + 1, std::memory_order_seq_cst);
        if (vpProducerWaiting.load(std::memory_order_seq_cst))
        {
            std::lock_guard<std::mutex> lock(vpProducerMutex);
            vpProducerCv.notify_one();
        }
        return ev;
    }

    static std::vector<std::vector<uint8_t>> s_vpKeep; // PERF-MG: parser-owned packets referenced by queued VP_GIF2 events (inner buffers never move)
    static std::atomic<uint64_t> s_vpStWhy[5]; // PROBE-MG: drains by site (end/mskpath3/mpg/directimage/syncunpack)
    static void vpDrain(int why)
    {
        vpFlushPendingWrites(); // PERF-V1H
        if (vpStatOn()) s_vpStWhy[why % 5].fetch_add(1, std::memory_order_relaxed);
        vpEnsureThreadStarted();
        const bool dual = vpDualOn();
        if (dual)
            vpEnsureThread1Started();
        const uint32_t seq = ++vpFlushSeqGen;
        VpEvent ev{};
        ev.kind = VP_FLUSH;
        ev.a = seq;
        vpPushEvent(ev);

        const uint64_t dwT0 = vpStatOn() ? __rdtsc() : 0ull; // PROBE-MG
        {
        const uint64_t vwT = vw::on() ? __rdtsc() : 0ull; // PROBE-VW
        std::unique_lock<std::mutex> lock(vpFlushMutex);
        vpFlushCv.wait(lock, [seq, dual]() {
            const bool idle0 = static_cast<int32_t>(vpIdleSeq.load(std::memory_order_acquire) - seq) >= 0;
            if (!dual)
                return idle0;
            const bool idle1 = static_cast<int32_t>(vpIdleSeq1.load(std::memory_order_acquire) - seq) >= 0;
            return idle0 && idle1;
        });
        if (vwT) vwBlk(true, vwT); // PROBE-VW
        }
        if (dual)
        {
            if (s_vpVu1Data)
            {
                if (!vpVerifyOn() && s_vpMscalK > 0u && ((s_vpMscalK - 1u) & 1u) == 1u)
                {
                    std::memcpy(s_vpVu1Data, s_vpShadow, PS2_VU1_DATA_SIZE);
                }
                else
                {
                    std::memcpy(s_vpShadow, s_vpVu1Data, PS2_VU1_DATA_SIZE);
                }
            }
            s_vpMscalK = 0u;
            s_vpTurnT = 0u;
            s_vpTurnDone.store(0u, std::memory_order_release);
            for (int i = 0; i < 256; ++i)
            {
                s_vpSlots[i].done.store(0u, std::memory_order_relaxed);
                s_vpSlots[i].entryReady.store(false, std::memory_order_relaxed);
                s_vpSlots[i].gif.clear();
                s_vpSlots[i].dq.clear();
                s_vpSlots[i].dv.clear();
            }
            vpVerifyDone.store(0u, std::memory_order_release);
            if (vpVerifyOn())
                vpVerifySummaryPrint();
        }
        if (dwT0) // PROBE-MG
        {
            s_vpStDrainWait.fetch_add(__rdtsc() - dwT0, std::memory_order_relaxed);
            s_vpStDrains.fetch_add(1, std::memory_order_relaxed);
            static uint64_t s_last = 0;
            const uint64_t now = __rdtsc();
            if (now - s_last > 6000000000ull)
            {
                const double ms = 1.0 / 3.0e6; // ~3 GHz TSC: ticks -> ms (approximate)
                const double sec = s_last ? static_cast<double>(now - s_last) / 3.0e9 : 2.0;
                s_last = now;
                const double invSec = sec > 0.0 ? (1.0 / sec) : 1.0;
                const uint64_t applyQwSec = static_cast<uint64_t>(s_vpStApplyQw.exchange(0) * invSec);
                const uint64_t diffQwSec = static_cast<uint64_t>(s_vpStDiffQw.exchange(0) * invSec);
                ::ps2xSafeLog("[vpipe] drains=%llu drainwait_ms=%.0f exec_busy_ms=%.0f x1_busy_ms=%.0f exec_spin_ms=%.0f sleeps=%llu full=%llu ev[w/mscal/mscnt/gif/flush]=%llu/%llu/%llu/%llu/%llu x1_runs=%llu spec=%llu reruns=%llu waits_exact=%llu applyQw=%llu diffQw=%llu\n",
                              (unsigned long long)s_vpStDrains.exchange(0), s_vpStDrainWait.exchange(0) * ms, s_vpStBusy.exchange(0) * ms,
                              s_vpStBusy1.exchange(0) * ms,
                              s_vpStSpin.exchange(0) * ms, (unsigned long long)s_vpStSleeps.exchange(0), (unsigned long long)s_vpStFull.exchange(0),
                              (unsigned long long)s_vpStEv[1].exchange(0), (unsigned long long)s_vpStEv[2].exchange(0), (unsigned long long)s_vpStEv[3].exchange(0),
                              (unsigned long long)s_vpStEv[4].exchange(0), (unsigned long long)s_vpStEv[5].exchange(0),
                              (unsigned long long)s_vpStX1Runs.exchange(0), (unsigned long long)s_vpStSpec.exchange(0),
                              (unsigned long long)s_vpStReruns.exchange(0), (unsigned long long)s_vpStWaitsExact.exchange(0),
                              (unsigned long long)applyQwSec, (unsigned long long)diffQwSec);
                ::ps2xSafeLog("[vpipe] drains by site end/mskpath3/mpg/directimage/syncunpack=%llu/%llu/%llu/%llu/%llu\n",
                              (unsigned long long)s_vpStWhy[0].exchange(0), (unsigned long long)s_vpStWhy[1].exchange(0), (unsigned long long)s_vpStWhy[2].exchange(0),
                              (unsigned long long)s_vpStWhy[3].exchange(0), (unsigned long long)s_vpStWhy[4].exchange(0));
                { // PROBE-DB3H
                    uint32_t cnt[1024];
                    for (uint32_t i = 0; i < 1024u; ++i) cnt[i] = s_db3NonKickHist[i].exchange(0, std::memory_order_relaxed);
                    char buf[512]; size_t n = 0; buf[0] = 0;
                    for (int t = 0; t < 16; ++t)
                    {
                        uint32_t best = 0, bi = 0;
                        for (uint32_t i = 0; i < 1024u; ++i) if (cnt[i] > best) { best = cnt[i]; bi = i; }
                        if (!best) break;
                        n += static_cast<size_t>(std::snprintf(buf + n, sizeof(buf) - n, " %u:%u", bi, best));
                        cnt[bi] = 0;
                    }
                    ::ps2xSafeLog("[vpipe] nonkick progs=%llu top:%s\n", (unsigned long long)s_db3NonKickProgs.exchange(0), buf);
                }
            }
        }
    }

    static void vpPushRec(const VpRec &rec)
    {
        // PERF-MG2: the exec thread never reads vpRecTail (records are published by the VP_WRITES event's seq_cst store), so the tail is a
        // relaxed producer-local counter, and the consumer head is only re-read when the cached value says the ring is full.
        const bool dual = vpDualOn();
        static uint32_t s_headCache = 0; // parser thread only
        const uint32_t tail = vpRecTail.load(std::memory_order_relaxed);
        if (tail - s_headCache >= kVpRecCap)
        {
            const uint32_t lag = vpRecLag(tail, dual);
            s_headCache = tail - lag;
        }
        if (tail - s_headCache >= kVpRecCap)
        {
            vpFlushPendingWrites(); // PERF-V1H: publish pending writes before waiting on consumer to prevent deadlock
            const uint64_t vwT = vw::on() ? __rdtsc() : 0ull; // PROBE-VW
            struct VwG { uint64_t t; ~VwG() { if (t) vwBlk(false, t); } } vwG{vwT}; // PROBE-VW
            for (int spin = 0; spin < vpSpinLimit(); ++spin)
            {
                if (vpRecLag(tail, dual) < kVpRecCap)
                    break;
                _mm_pause();
            }
            if (vpRecLag(tail, dual) >= kVpRecCap)
            {
                if (vpStatOn()) s_vpStFull.fetch_add(1, std::memory_order_relaxed); // PROBE-MG
                std::unique_lock<std::mutex> lock(vpRecProducerMutex);
                vpRecProducerWaiting.store(true, std::memory_order_seq_cst);
                vpRecProducerCv.wait(lock, [&]() {
                    return vpRecLag(tail, dual) < kVpRecCap;
                });
                vpRecProducerWaiting.store(false, std::memory_order_seq_cst);
            }
            s_headCache = tail - vpRecLag(tail, dual);
        }
        vpRecRing[tail & (kVpRecCap - 1)] = rec;
        vpRecTail.store(tail + 1, std::memory_order_relaxed);
    }

    static void vpExecThreadFunc()
    {
        ThreadNaming::SetCurrentThreadName("Vu1Exec");
        if (const char *pe = std::getenv("PS2X_PROF_VU1EXEC"); pe && pe[0] == '1') // PROBE-MG: point the sampler (PS2X_PROF_SAMPLE=1) at this thread
            probe_ae::recordMainGuestThread();
        ps2_vif1_mark_worker_thread();
        ps2_gs_async_batch(true);
        const bool vxOn = ::ps2xVxOn(); // PROBE-VX
        if (vxOn) ::ps2xVxMarkThread(); // PROBE-VX
        const bool statOn = vpStatOn(); // PERF-V1G
        const bool dualOn = vpDualOn(); // PERF-V1G
        const bool verifyOn = vpVerifyOn(); // PERF-V1G
        if (!dualOn) ::ps2xVu1Path1Batch(true); // PERF-V1I
        for (;;)
        {
            const uint64_t vxA = vxOn ? __rdtsc() : 0ull; // PROBE-VX
            VpEvent ev = vpPopEvent();
            const uint64_t vxB = vxOn ? __rdtsc() : 0ull; // PROBE-VX
            if (vxOn) ::ps2xVxIdle(vxB - vxA); // PROBE-VX
            const uint64_t buT0 = statOn ? __rdtsc() : 0ull; // PROBE-MG // PERF-V1G
            struct BusyAcct { uint64_t t0; uint8_t k; ~BusyAcct() { if (t0) { s_vpStBusy.fetch_add(__rdtsc() - t0, std::memory_order_relaxed); s_vpStEv[k & 7].fetch_add(1, std::memory_order_relaxed); } } } busyAcct{buT0, ev.kind};
            switch (ev.kind)
            {
            case VP_MSCAL:
                if (dualOn) // PERF-V1G
                {
                    if (verifyOn) // PERF-V1G
                        vpRunVerifyX0(ev);
                    else
                        vpRunMscal(ev, 0);
                }
                else
                {
                    if (vpMscalFn) // PERF-V1G
                        vpMscalFn(vpMscalCtx, ev.a, ev.b, ev.c);
                }
                break;
            case VP_MSCNT:
                ::ps2xVu1FlushPath1(); // PERF-V1I
                if (vpMscntCb)
                    vpMscntCb(ev.b, ev.c);
                break;
            case VP_GIF2:
                ::ps2xVu1FlushPath1(); // PERF-V1I
                if (dualOn) // PERF-V1G
                {
                    if (verifyOn) // PERF-V1G
                    {
                        if (vpGif2Cb)
                            vpGif2Cb(ev.p, ev.n, ev.hl != 0);
                    }
                    else
                    {
                        vpWaitCond([&]() {
                            return s_vpTurnDone.load(std::memory_order_acquire) == ev.t;
                        });
                        if (vpGif2Cb)
                            vpGif2Cb(ev.p, ev.n, ev.hl != 0);
                        ps2_gs_async_batch(false);
                        ps2_gs_async_batch(true);
                        s_vpTurnDone.store(ev.t + 1u, std::memory_order_release);
                    }
                }
                else
                {
                    if (vpGif2Cb)
                        vpGif2Cb(ev.p, ev.n, ev.hl != 0);
                }
                break;
            case VP_WRITES:
                if (s_vpVu1Data)
                {
                    if (dualOn) // PERF-V1H: s_tlVifWrites is only read by vpRunMscal in dual/verify mode
                    {
                        for (uint32_t i = 0u; i < ev.b; ++i)
                        {
                            const VpRec &rec = vpRecRing[(ev.a + i) & (kVpRecCap - 1)];
                            if (rec.qw < 1024u)
                                s_tlVifWrites.set(rec.qw);
                            uint8_t *dst = s_vpVu1Data + (static_cast<size_t>(rec.qw) * 16u);
                            if (rec.mask == 0x0Fu)
                            {
                                std::memcpy(dst, rec.lanes, 16);
                            }
                            else
                            {
                                uint32_t *dst32 = reinterpret_cast<uint32_t *>(dst);
                                for (uint32_t f = 0u; f < 4u; ++f)
                                {
                                    if (rec.mask & (1u << f))
                                        dst32[f] = rec.lanes[f];
                                }
                            }
                        }
                    }
                    else
                    {
                        // PERF-V1H: split into mask==0xF fast path (16-byte copy) and masked path, no s_tlVifWrites overhead
                        uint32_t i = 0u;
                        while (i < ev.b)
                        {
                            for (; i < ev.b; ++i)
                            {
                                const VpRec &rec = vpRecRing[(ev.a + i) & (kVpRecCap - 1)];
                                if (rec.mask != 0x0Fu)
                                    break;
                                std::memcpy(s_vpVu1Data + (static_cast<size_t>(rec.qw) * 16u), rec.lanes, 16);
                            }
                            for (; i < ev.b; ++i)
                            {
                                const VpRec &rec = vpRecRing[(ev.a + i) & (kVpRecCap - 1)];
                                if (rec.mask == 0x0Fu)
                                    break;
                                uint32_t *dst32 = reinterpret_cast<uint32_t *>(s_vpVu1Data + (static_cast<size_t>(rec.qw) * 16u));
                                const uint8_t m = rec.mask;
                                if (m & 1u) dst32[0] = rec.lanes[0];
                                if (m & 2u) dst32[1] = rec.lanes[1];
                                if (m & 4u) dst32[2] = rec.lanes[2];
                                if (m & 8u) dst32[3] = rec.lanes[3];
                            }
                        }
                    }
                }
                vpRecHead.store(ev.a + ev.b, std::memory_order_seq_cst);
                if (vpRecProducerWaiting.load(std::memory_order_seq_cst))
                {
                    std::lock_guard<std::mutex> lock(vpRecProducerMutex);
                    vpRecProducerCv.notify_all();
                }
                break;
            case VP_FLUSH:
                ::ps2xVu1FlushPath1(); // PERF-V1I
                if (dualOn) // PERF-V1H
                {
                    s_tlVifWrites.reset();
                    s_tlVifAfterPrev.reset();
                }
                ps2_gs_async_batch(false);
                ps2_gs_async_batch(true);
                if (g_ps2Ft2On) ps2_ft2_ev(4, ev.a); // PROBE-FT2: EXEC_LAST (Vu1Exec reached the drain flush)
                vpIdleSeq.store(ev.a, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(vpFlushMutex);
                    vpFlushCv.notify_all();
                }
                break;
            default:
                ::ps2xVu1FlushPath1(); // PERF-V1I
                break;
            }
            if (vxOn) ::ps2xVxEv(static_cast<uint32_t>(ev.kind), __rdtsc() - vxB); // PROBE-VX
        }
    }

    static void vpExec2ThreadFunc()
    {
        ThreadNaming::SetCurrentThreadName("Vu1Exec2");
        ps2_vif1_mark_worker_thread();
        ps2_gs_async_batch(true);
        for (;;)
        {
            VpEvent ev = vpPopEvent1();
            switch (ev.kind)
            {
            case VP_WRITES:
            {
                const uint64_t t0 = vpStatOn() ? __rdtsc() : 0ull;
                for (uint32_t i = 0u; i < ev.b; ++i)
                {
                    const VpRec &rec = vpRecRing[(ev.a + i) & (kVpRecCap - 1)];
                    if (rec.qw < 1024u)
                        s_tlVifWrites.set(rec.qw);
                    uint8_t *dst = s_vpShadow + (static_cast<size_t>(rec.qw) * 16u);
                    if (rec.mask == 0x0Fu)
                    {
                        std::memcpy(dst, rec.lanes, 16);
                    }
                    else
                    {
                        uint32_t *dst32 = reinterpret_cast<uint32_t *>(dst);
                        for (uint32_t f = 0u; f < 4u; ++f)
                        {
                            if (rec.mask & (1u << f))
                                dst32[f] = rec.lanes[f];
                        }
                    }
                }
                if (t0)
                    s_vpStBusy1.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
                vpRecHead1.store(ev.a + ev.b, std::memory_order_seq_cst);
                if (vpRecProducerWaiting.load(std::memory_order_seq_cst))
                {
                    std::lock_guard<std::mutex> lock(vpRecProducerMutex);
                    vpRecProducerCv.notify_all();
                }
                break;
            }
            case VP_FLUSH:
                s_tlVifWrites.reset();
                s_tlVifAfterPrev.reset();
                ps2_gs_async_batch(false);
                ps2_gs_async_batch(true);
                vpIdleSeq1.store(ev.a, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(vpFlushMutex);
                    vpFlushCv.notify_all();
                }
                break;
            case VP_MSCAL:
            {
                const uint64_t t0 = vpStatOn() ? __rdtsc() : 0ull;
                if (vpVerifyOn())
                    vpRunVerifyX1(ev);
                else
                    vpRunMscal(ev, 1);
                if (t0)
                    s_vpStBusy1.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
                break;
            }
            case VP_MSCNT:
            case VP_GIF2:
            default:
                break;
            }
        }
    }
} // namespace

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    VsTimer vsFn(&g_vs.fn);
    if (vifStatOn())
    {
        ++g_vs.calls;
        vifStatReport();
    }

    s_erCurrentChunk = data;
    s_erCurrentChunkSize = sizeBytes;
    s_erCurrentChunkReported = false;

    probe_ae::onBhVif1Start();
    probe_ae::onBxKick();
    // PERF-CU: the MSCAL scan, the clock reads and the movie bookkeeping only feed probes / the movie HLE
    const bool cuProbe = probe_ae::isEnabled();
    static const bool s_cuMovie = probe_ae::isMovieHleActive() || probe_ae::isMovieHleVerify();
    // PERF-XS: the scan was 2.7% of the VU1 worker outside movies (HLE is on by default): its result is only read by the movie HLE, which acts only while a movie is active
    uint32_t mscal0x48Count = (cuProbe || (s_cuMovie && probe_ae::isMovieActivePublic())) ? count0x48MscalsInKick(data, sizeBytes) : 0u;
    probe_ae::onMovieKickMscalCount(mscal0x48Count);
    uint32_t kickUnpacksSkipped = 0;
    uint32_t kickBytesSkipped = 0;
    const auto tVif1Start = cuProbe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    static const bool s_vif1Pipe = []() {
        const char *e = std::getenv("PS2X_VIF1_PIPE");
        return !(e && e[0] == '0'); // default ON since A/B mg5 (silent: fps 39.3 -> 43.0); =0 disables
    }();
    const bool vpOn = s_vif1Pipe &&
                      ps2_vif1_on_worker() &&
                      !probe_ae::isEnabled() &&
                      !isVu3dProbeEnabled() &&
                      !probe_ae::isDcProbeEnabled() &&
                      !(s_cuMovie && probe_ae::isMovieActivePublic()) &&
                      !isUnpackVerifyEnabled();

    if (vpOn)
    {
        s_vpVu1Data = m_vu1Data;
        vpEnsureThreadStarted();
        const bool dualOn = vpDualOn();
        if (dualOn)
        {
            vpEnsureThread1Started();
            vpGif1Cb = [this](const uint8_t *p, uint32_t n) {
                submitGifPacket(GifPathId::Path1, p, n, true, false);
            };
        }
        vpMscalCtx = this; // PERF-V1G
        vpMscalFn = [](void *ctx, uint32_t pc, uint32_t top, uint32_t itop) { // PERF-V1G
            auto *self = static_cast<PS2Memory *>(ctx);
            if (self->m_vu1MscalCallback)
                self->m_vu1MscalCallback(pc, top, itop);
        };
        vpMscalCb = [this](uint32_t pc, uint32_t top, uint32_t itop) {
            if (m_vu1MscalCallback)
                m_vu1MscalCallback(pc, top, itop);
        };
        vpMscntCb = [this](uint32_t top, uint32_t itop) {
            if (m_vu1MscntCallback)
                m_vu1MscntCallback(top, itop);
        };
        vpGif2Cb = [this](const uint8_t *p, uint32_t n, bool hl) {
            submitGifPacket(GifPathId::Path2, p, n, true, hl);
        };
    }
    struct VpDrainGuard {
        bool on;
        ~VpDrainGuard() {
            if (on)
            {
                vpFlushPendingWrites(); // PERF-V1H
                vpDrain(0);
                s_vpKeep.clear(); // PERF-MG: exec is idle, queued image packets are consumed
            }
        }
    } vpGuard{vpOn};

#if defined(PS2X_PROBE_AE) || 1
    if (probe_ae::isEnabled()) {
        probe_ae::onVif1DmaStart(sizeBytes);
        probe_ae::onAwVifKickStart();
    }
#endif

    uint32_t pos = 0;
    const bool vwOn = vpOn && vw::on(); // PROBE-VW

    while (pos + 4 <= sizeBytes)
    {

        if (vifStatOn())
            ++g_vs.codes;
        uint32_t pos_cmd = pos;
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;
        uint32_t pos_payload = pos;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;
        vw::Iter vwIter(vwOn, opcode, num, imm); // PROBE-VW

        if (m_vu1Data && isVu3dProbeEnabled())
        {
            // Task FI2: who changes VU1 data qw 476..479 (raw DMA-chain-looking words)? attribute to previous VIFcode
            static uint32_t s_fiShadow[16];
            static uint32_t s_fiPrevCmd = 0, s_fiPrevPos = 0, s_fiPrevSize = 0;
            static const uint8_t *s_fiPrevData = nullptr;
            static std::atomic<uint32_t> s_fiLines{0};
            uint32_t cur[16];
            std::memcpy(cur, m_vu1Data + 476u * 16u, sizeof(cur));
            if (std::memcmp(cur, s_fiShadow, sizeof(cur)) != 0)
            {
                if ((cur[1] > 0x1000u && cur[1] < 0x2000000u) && s_fiLines.fetch_add(1, std::memory_order_relaxed) < 40u)
                {
                    const bool inRdram = m_rdram && s_fiPrevData >= m_rdram && s_fiPrevData < m_rdram + PS2_RAM_SIZE;
                    std::printf("[probe] fi chg prev_cmd=0x%08X prev_pos=%u chunk_size=%u chunk_rdram=0x%08X now_cmd=0x%08X now_pos=%u qw476=%08X %08X %08X %08X qw477=%08X %08X %08X %08X\n",
                                s_fiPrevCmd, s_fiPrevPos, s_fiPrevSize,
                                inRdram ? static_cast<uint32_t>(s_fiPrevData - m_rdram) : 0xFFFFFFFFu, cmd, pos_cmd,
                                cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7]);
                    for (uint32_t probePos : {s_fiPrevPos + 4u, s_fiPrevPos + 4u + 16u * 40u, s_fiPrevPos + 4u + 16u * 79u})
                    {
                        uint32_t spSrc = 0, spBytes = 0, spOff = 0;
                        if (ps2_fk_lookup(s_fiPrevSize, probePos, &spSrc, &spBytes, &spOff))
                            std::printf("[probe] fi span pos=%u src=0x%08X span_bytes=%u span_off=%u\n", probePos, spSrc, spBytes, spOff);
                        else
                            std::printf("[probe] fi span pos=%u src=? (no span)\n", probePos);
                    }
                    std::fflush(stdout);
                }
                std::memcpy(s_fiShadow, cur, sizeof(cur));
            }
            s_fiPrevCmd = cmd;
            s_fiPrevPos = pos_cmd;
            s_fiPrevSize = sizeBytes;
            s_fiPrevData = data;
        }

        struct VcScopeGuard {
            uint32_t pos_cmd;
            uint32_t cmd;
            uint8_t num;
            uint16_t imm;
            bool irq;
            uint32_t pos_payload;
            const uint32_t &pos;
            ~VcScopeGuard() {
#if defined(PS2X_PROBE_AE) || 1
                if (probe_ae::isEnabled()) {
                    uint32_t consumed = (pos > pos_payload) ? (pos - pos_payload) : 0u;
                    probe_ae::onAwVifCode(pos_cmd, cmd, num, imm, irq, consumed);
                }
#endif
            }
        } vcGuard{pos_cmd, cmd, num, imm, irq, pos_payload, pos};

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
        {
            vif1_regs.stat |= (1u << 10); // VIS
            vif1_regs.stat |= (1u << 11); // INT
            recordCompletedIntcCause(5u);
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onVif1Irq();
                probe_ae::onCause5Raise();
            }
#endif
            static int s_traceVif = -1;
            if (s_traceVif == -1) {
                const char *env = std::getenv("PS2X_TRACE_VIF");
                s_traceVif = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0;
            }
            if (s_traceVif) {
                std::fprintf(stderr, "[vif1:irq] cmd=0x%08x opcode=0x%02x imm=0x%04x\n", cmd, opcode, imm);
                std::fflush(stderr);
            }
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            recordEpEvent("OFFSET val=0x%X tops=%u", imm & 0x3FFu, vif1_regs.tops & 0x3FFu);
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            recordEpEvent("BASE val=0x%X", imm & 0x3FFu);
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            recordEpEvent("STMOD mode=%u", vif1_regs.mode);
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            if (vpOn)
                vpDrain(1);
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            recordEpEvent("MSKPATH3 mask=%u", (imm & 0x8000u) ? 1u : 0u);
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            if (vpOn)
            {
                ps2_gs_async_batch(false);
                ps2_gs_async_batch(true);
            }
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            if (!ps2_vif1_on_worker()) // PERF-HN: m_ioRegisters is an unsynchronised map; the MARK read uses vif1_regs.mark
                m_ioRegisters[0x10003C30u] = imm;
            recordCompletedIntcCause(5u);
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onVif1Mark(imm);
                probe_ae::onCause5Raise();
                if (imm == 0x2000u) {
                    probe_ae::onAwMark2000();
                }
            }
#endif
            std::fprintf(stderr, "[vif1:mark] imm=0x%04x irq=%d\n", imm, irq ? 1 : 0);
            std::fflush(stderr);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            recordEpEvent("FLUSH op=0x%02X", opcode);
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            recordEpEvent("%s pc=0x%03X top=%u itop=%u tops=%u",
                          (opcode == VIF_MSCAL ? "MSCAL" : "MSCALF"),
                          startPC, runTop, runItop, vif1_regs.tops & 0x3FFu);

#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onBbVif1Mscal(startPC, runItop, runTop, runTop,
                                        vif1_regs.base & 0x3FFu, vif1_regs.ofst & 0x3FFu,
                                        vif1_regs.cycle & 0xFFu, (vif1_regs.cycle >> 8) & 0xFFu,
                                        vif1_regs.mode & 3u);
            }
#endif

            const bool skipMscal = probe_ae::isMovieSkipUnpackEnabled() && probe_ae::isMovieKickNative();
            if (vpOn)
            {
                if (!skipMscal && m_vu1MscalCallback)
                {
                    VpEvent ev{};
                    ev.kind = VP_MSCAL;
                    ev.a = startPC;
                    ev.b = runTop;
                    ev.c = runItop;
                    if (vpDualOn())
                    {
                        ev.n = s_vpMscalK++;
                        ev.t = s_vpTurnT++;
                    }
                    vpPushEvent(ev);
                }
            }
            else if (!skipMscal && m_vu1MscalCallback) {
                const auto tVu1Start = cuProbe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                {
                    VsTimer vsVu(&g_vs.vu);
                    if (vifStatOn())
                        ++g_vs.mscal;
                    m_vu1MscalCallback(startPC, runTop, runItop);
                }
#if defined(PS2X_PROBE_AE) || 1
                if (probe_ae::isEnabled()) {
                    auto tVu1End = std::chrono::steady_clock::now();
                    probe_ae::onAyTimeVu1(std::chrono::duration_cast<std::chrono::microseconds>(tVu1End - tVu1Start).count());
                }
#endif
            }
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            recordEpEvent("MSCNT top=%u itop=%u tops=%u",
                          runTop, runItop, vif1_regs.tops & 0x3FFu);

            probe_ae::flushMovieBatch(nullptr, this);
            if (vpOn)
            {
                if (m_vu1MscntCallback)
                {
                    if (vpDualOn())
                        vpDrain(0);
                    VpEvent ev{};
                    ev.kind = VP_MSCNT;
                    ev.b = runTop;
                    ev.c = runItop;
                    vpPushEvent(ev);
                }
            }
            else if (m_vu1MscntCallback) {
                const auto tVu1Start = cuProbe ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                {
                    VsTimer vsVu(&g_vs.vu);
                    if (vifStatOn())
                        ++g_vs.mscal;
                    m_vu1MscntCallback(runTop, runItop);
                }
#if defined(PS2X_PROBE_AE) || 1
                if (probe_ae::isEnabled()) {
                    auto tVu1End = std::chrono::steady_clock::now();
                    probe_ae::onAyTimeVu1(std::chrono::duration_cast<std::chrono::microseconds>(tVu1End - tVu1Start).count());
                }
#endif
            }
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            recordEpEvent("STMASK mask=0x%08X", maskValue);
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            recordEpEvent("STROW row=%08X %08X %08X %08X",
                          vif1_regs.row[0], vif1_regs.row[1], vif1_regs.row[2], vif1_regs.row[3]);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            recordEpEvent("STCOL col=%08X %08X %08X %08X",
                          vif1_regs.col[0], vif1_regs.col[1], vif1_regs.col[2], vif1_regs.col[3]);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            if (vpOn)
                vpDrain(2);
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            recordEpEvent("MPG addr=0x%X num=%u", destAddr, instructionCount);
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            if (vpOn)
            {
                ps2_gs_async_batch(false);
                ps2_gs_async_batch(true);
            }
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            VsTimer vsDirect(&g_vs.direct);
            probe_ae::flushMovieBatch(nullptr, this);
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (vifStatOn())
            {
                ++g_vs.directs;
                g_vs.directBytes += static_cast<uint64_t>(std::min(qwCount, availableQw)) * 16u;
            }
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                if (m_vif1PendingPath2ImageQwc != 0u)
                {
                    const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwCount);
                    std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
                    const uint64_t imageTag =
                        static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                        ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                        (static_cast<uint64_t>(kGifFmtImage) << 58);
                    std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
                    std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
                    if (vpOn)
                    {
                        // PERF-MG: keep the built packet alive until the end-of-job drain and queue it in order (was a drain per chunk, ~200/frame)
                        s_vpKeep.push_back(std::move(imagePacket));
                        VpEvent ev{};
                        ev.kind = VP_GIF2;
                        ev.hl = directHl ? 1 : 0;
                        ev.p = s_vpKeep.back().data();
                        ev.n = static_cast<uint32_t>(s_vpKeep.back().size());
                        if (vpDualOn())
                            ev.t = s_vpTurnT++;
                        vpPushEvent(ev);
                    }
                    else
                        submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, directHl);
                    m_vif1PendingPath2ImageQwc -= chunkQw;
                    if (m_vif1PendingPath2ImageQwc == 0u)
                    {
                        m_vif1PendingPath2DirectHl = false;
                    }
                }
                else
                {
                    if (vpOn)
                    {
                        VpEvent ev{};
                        ev.kind = VP_GIF2;
                        ev.hl = directHl ? 1 : 0;
                        ev.p = data + pos;
                        ev.n = qwCount * 16;
                        if (vpDualOn())
                            ev.t = s_vpTurnT++;
                        vpPushEvent(ev);
                    }
                    else
                    {
                        submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);
                    }

                    const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                    if (pendingImageQw != 0u)
                    {
                        m_vif1PendingPath2ImageQwc = pendingImageQw;
                        m_vif1PendingPath2DirectHl = directHl;
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            VsTimer vsUnpack(&g_vs.unpack);
            VsShapeTimer vsShape;
            if (vifStatOn())
                ++g_vs.unpacks;
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u && wl == 0u)
            {
                cl = 1u;
                wl = 1u;
            }
            else if (wl == 0u)
            {
                wl = 1u;
            }

            vsShape.set((opcode & 0x7Fu) | (cl << 7) | (wl << 15) | ((maskEnable ? 1u : 0u) << 23) | ((vif1_regs.mode & 3u) << 24) | (((imm >> 14) & 1u) << 26) | (((imm >> 15) & 1u) << 27), writeVectorCount);
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const uint32_t totalSpan = ((writeVectorCount - 1u) / wl) * cl + ((writeVectorCount - 1u) % wl);
            const uint64_t lastVec = static_cast<uint64_t>(vuAddr) + totalSpan;
            const bool rangeOk = (lastVec < 0x400u) && ((lastVec + 1u) * 16u <= PS2_VU1_DATA_SIZE);
            const bool canFastPath = (cl >= wl && sourceVectorCount == writeVectorCount &&
                                      vn <= 3u && vl <= 2u &&
                                      ((vif1_regs.mode & 3u) == 0u) && rangeOk);
            const bool destWrap = (cl >= wl) ? (vuAddr + totalSpan >= 0x400u) : (vuAddr + writeVectorCount > 0x400u);
            const bool skipUnpack = s_skipUnpackEnabled && probe_ae::isMovieKickNative();
            const bool syncUnpack = skipUnpack || (vl == 3u && vn != 3u) || destWrap;
            if (vpOn && syncUnpack)
                vpDrain(4);

            const uint8_t *srcBaseAt974 = (pos <= sizeBytes) ? (data + pos) : nullptr;
            uint32_t srcRdram = 0xFFFFFFFFu;
            if (m_rdram && srcBaseAt974 && srcBaseAt974 >= m_rdram && srcBaseAt974 < m_rdram + PS2_RAM_SIZE)
            {
                srcRdram = static_cast<uint32_t>(srcBaseAt974 - m_rdram);
            }
            uint32_t w0[4]{0, 0, 0, 0};
            uint32_t avail = (pos <= sizeBytes) ? (sizeBytes - pos) : 0u;
            if (avail >= 16u && srcBaseAt974)
            {
                std::memcpy(w0, srcBaseAt974, 16);
            }
            else if (avail > 0u && srcBaseAt974)
            {
                std::memcpy(w0, srcBaseAt974, avail);
            }

            if (isVu3dProbeEnabled()) // PERF-UV: 24 variadic arguments were pushed for every UNPACK even when the probe is off
            recordEpEvent("UNPACK cmd=0x%02X dest=%u num=%u cl=%u wl=%u flg=%u tops=%u src=0x%08X w0=%08X %08X %08X %08X mask=%08X mode=%u row=%08X %08X %08X %08X col=%08X %08X %08X %08X",
                          opcode, vuAddr, writeVectorCount,
                          cl, wl, (imm & 0x8000u) ? 1u : 0u, vif1_regs.tops & 0x3FFu,
                          srcRdram, w0[0], w0[1], w0[2], w0[3],
                          vif1_regs.mask, vif1_regs.mode & 3u,
                          vif1_regs.row[0], vif1_regs.row[1], vif1_regs.row[2], vif1_regs.row[3],
                          vif1_regs.col[0], vif1_regs.col[1], vif1_regs.col[2], vif1_regs.col[3]);

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            if (!s_unpackQuiet && cl == 0u && probe_ae::isDcProbeEnabled())
            {
                static std::atomic<uint32_t> s_dc16Cl0Count{0};
                uint32_t cnt = s_dc16Cl0Count.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 8u)
                {
                    std::printf("[probe] dc16 cl0 unpack cmd=0x%02X num=%u wl=%u dest=0x%03X mask=0x%08X row0=0x%08X bytes_consumed=%u\n",
                                opcode, num, wl, vuAddr, vif1_regs.mask, vif1_regs.row[0], totalBytes);
                    std::fflush(stdout);
                }
            }
#if defined(PS2X_PROBE_AE) || 1
            if (!s_unpackQuiet && (probe_ae::isEnabled() || probe_ae::isMovieHleFrameMode() || probe_ae::isMovieHleVerify())) {
                uint8_t fmt = opcode & 0x0Fu;
                uint32_t usn = (imm & 0x4000u) != 0u ? 1u : 0u;
                uint32_t flg = (imm & 0x8000u) != 0u ? 1u : 0u;
                uint32_t addr = imm & 0x3FFu;
                uint32_t computed_words = totalBytes / 4u;
                if (probe_ae::isEnabled()) {
                    probe_ae::onAwUnpack(fmt, num, cl, wl, usn, flg, addr, computed_words);
                    probe_ae::onAyUnpackCheck(fmt, addr, num, pos_payload, data + pos_payload, sizeBytes);
                }
                probe_ae::onBbRecordUnpack(pos_payload, totalBytes, vuAddr, fmt, num);
            }
#endif

            if (skipUnpack)
            {
                kickUnpacksSkipped++;
                kickBytesSkipped += totalBytes;
            }
            else if (m_vu1Data && (totalBytes > 0 || cl == 0u) && pos + totalBytes <= sizeBytes)
            {
                const bool vpAsync = vpOn && !syncUnpack;
                if (isVu3dProbeEnabled())
                {
                    s_erTaken[opcode & 0x7Fu].fetch_add(1, std::memory_order_relaxed);
                }
                const uint8_t *srcBase = data + pos;

                // PERF-VS: the fast/generic counters only feed the probe log lines; two atomic RMWs per UNPACK (1.4M/s) are skipped when no probe is on
                static std::atomic<uint64_t> s_ciFastCount{0};
                static std::atomic<uint64_t> s_ciGenericCount{0};
                static std::atomic<uint64_t> s_ciMismCount{0};
                static std::atomic<uint32_t> s_ciMismLogged{0};
                static std::chrono::steady_clock::time_point s_ciLastSecLog{};

                struct DbShapeKey {
                    uint8_t vn, vl, cl, wl, mask, mode, flg, usn, num;
                    uint16_t vuAddr;
                    bool operator<(const DbShapeKey &o) const {
                        return std::tie(vn, vl, cl, wl, mask, mode, flg, usn, num, vuAddr) <
                               std::tie(o.vn, o.vl, o.cl, o.wl, o.mask, o.mode, o.flg, o.usn, o.num, o.vuAddr);
                    }
                };
                struct DbShapeStats {
                    uint64_t vecs = 0;
                    uint32_t calls = 0;
                };
                static std::map<DbShapeKey, DbShapeStats> s_dbGenericShapes;
                static std::atomic<uint64_t> s_dbFastVecs{0};
                static std::atomic<uint64_t> s_dbGenericVecs{0};

                auto ciLogMsg = [](const char *msg) {
                    static FILE *s_f = nullptr;
                    static bool s_tried = false;
                    if (!s_tried) {
                        s_tried = true;
                        s_f = std::fopen("logs/probe_bh_sink.txt", "a");
                    }
                    if (s_f) {
                        std::fputs(msg, s_f);
                        std::fflush(s_f);
                    }
                    std::printf("%s", msg);
                };

                Act act[4][4];
                uint32_t constVal[4][4];
                bool allDecoded = true;
                bool allProtect = true;

                if (canFastPath) {
                    const uint32_t maxC = std::min<uint32_t>(wl, 4u);
                    for (uint32_t c = 0u; c < maxC; ++c) {
                        for (uint32_t f = 0u; f < 4u; ++f) {
                            uint32_t spec = maskEnable ? ((vif1_regs.mask >> (((c * 4u) + f) * 2u)) & 0x3u) : 0u;
                            if (spec == 3u || (spec == 0u && vn != 0u && f >= static_cast<uint32_t>(components))) {
                                act[c][f] = ACT_PROTECT;
                                allDecoded = false;
                            } else if (spec == 1u) {
                                act[c][f] = ACT_CONST;
                                constVal[c][f] = vif1_regs.row[f];
                                allDecoded = false;
                                allProtect = false;
                            } else if (spec == 2u) {
                                act[c][f] = ACT_CONST;
                                constVal[c][f] = vif1_regs.col[c];
                                allDecoded = false;
                                allProtect = false;
                            } else {
                                act[c][f] = ACT_DECODED;
                                allProtect = false;
                            }
                        }
                    }
                    for (uint32_t c = maxC; c < 4u; ++c) {
                        for (uint32_t f = 0u; f < 4u; ++f) {
                            act[c][f] = act[3][f];
                            constVal[c][f] = constVal[3][f];
                        }
                    }
                }

                auto runFastPath = [&](uint8_t *targetVuData) {
                    if (allProtect) return;

                    if (allDecoded && cl == wl) {
                        uint32_t *dest = reinterpret_cast<uint32_t*>(targetVuData) + (vuAddr * 4u);
                        if (vl == 2u) {
                            if (vn == 0u) {
                                if (zeroExtend) {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        uint32_t s = static_cast<uint32_t>(srcBase[i]);
                                        dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                        dest += 4;
                                    }
                                } else {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        uint32_t s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcBase[i])));
                                        dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                        dest += 4;
                                    }
                                }
                            } else if (vn == 3u) {
                                if (zeroExtend) {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        const uint8_t *s = srcBase + i * 4u;
                                        dest[0] = s[0]; dest[1] = s[1]; dest[2] = s[2]; dest[3] = s[3];
                                        dest += 4;
                                    }
                                } else {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        const int8_t *s = reinterpret_cast<const int8_t*>(srcBase + i * 4u);
                                        dest[0] = static_cast<uint32_t>(static_cast<int32_t>(s[0]));
                                        dest[1] = static_cast<uint32_t>(static_cast<int32_t>(s[1]));
                                        dest[2] = static_cast<uint32_t>(static_cast<int32_t>(s[2]));
                                        dest[3] = static_cast<uint32_t>(static_cast<int32_t>(s[3]));
                                        dest += 4;
                                    }
                                }
                            }
                            return;
                        } else if (vl == 1u) {
                            if (vn == 0u) {
                                if (zeroExtend) {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        uint16_t raw = 0; std::memcpy(&raw, srcBase + i * 2u, 2);
                                        uint32_t s = static_cast<uint32_t>(raw);
                                        dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                        dest += 4;
                                    }
                                } else {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        int16_t raw = 0; std::memcpy(&raw, srcBase + i * 2u, 2);
                                        uint32_t s = static_cast<uint32_t>(static_cast<int32_t>(raw));
                                        dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                        dest += 4;
                                    }
                                }
                            } else if (vn == 3u) {
                                if (zeroExtend) {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        uint16_t r[4]; std::memcpy(r, srcBase + i * 8u, 8);
                                        dest[0] = r[0]; dest[1] = r[1]; dest[2] = r[2]; dest[3] = r[3];
                                        dest += 4;
                                    }
                                } else {
                                    for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                        int16_t r[4]; std::memcpy(r, srcBase + i * 8u, 8);
                                        dest[0] = static_cast<uint32_t>(static_cast<int32_t>(r[0]));
                                        dest[1] = static_cast<uint32_t>(static_cast<int32_t>(r[1]));
                                        dest[2] = static_cast<uint32_t>(static_cast<int32_t>(r[2]));
                                        dest[3] = static_cast<uint32_t>(static_cast<int32_t>(r[3]));
                                        dest += 4;
                                    }
                                }
                            }
                            return;
                        } else { // vl == 0u
                            if (vn == 0u) {
                                for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                    uint32_t s = 0; std::memcpy(&s, srcBase + i * 4u, 4);
                                    dest[0] = s; dest[1] = s; dest[2] = s; dest[3] = s;
                                    dest += 4;
                                }
                            } else if (vn == 3u) {
                                std::memcpy(dest, srcBase, writeVectorCount * 16u);
                            }
                            return;
                        }
                    }

                    if (s_uvOn)
                    {
                        alignas(16) uint32_t dmA[4][4] = {}, pmA[4][4] = {}, cvA[4][4] = {};
                        const uint32_t maxC = std::min<uint32_t>(wl, 4u);
                        for (uint32_t c = 0u; c < maxC; ++c)
                            for (uint32_t f = 0u; f < 4u; ++f)
                            {
                                dmA[c][f] = (act[c][f] == ACT_DECODED) ? 0xFFFFFFFFu : 0u;
                                pmA[c][f] = (act[c][f] == ACT_PROTECT) ? 0xFFFFFFFFu : 0u;
                                cvA[c][f] = (act[c][f] == ACT_CONST) ? constVal[c][f] : 0u;
                            }
                        __m128i dm[4], pm[4], cv[4];
                        for (uint32_t c = 0u; c < 4u; ++c)
                        {
                            const uint32_t k = std::min(c, maxC - 1u);
                            dm[c] = _mm_load_si128(reinterpret_cast<const __m128i *>(dmA[k]));
                            pm[c] = _mm_load_si128(reinterpret_cast<const __m128i *>(pmA[k]));
                            cv[c] = _mm_load_si128(reinterpret_cast<const __m128i *>(cvA[k]));
                        }
                        switch (vl)
                        {
                        case 0: vifUnpackVn<0>(vn, zeroExtend, targetVuData, srcBase, writeVectorCount, vuAddr, cl, wl, bytesPerVector, dm, cv, pm); break;
                        case 1: vifUnpackVn<1>(vn, zeroExtend, targetVuData, srcBase, writeVectorCount, vuAddr, cl, wl, bytesPerVector, dm, cv, pm); break;
                        default: vifUnpackVn<2>(vn, zeroExtend, targetVuData, srcBase, writeVectorCount, vuAddr, cl, wl, bytesPerVector, dm, cv, pm); break;
                        }
                        return;
                    }

                    const uint8_t *srcPtr = srcBase;
                    uint32_t destVec = vuAddr;
                    uint32_t cyclePos = 0;

                    for (uint32_t i = 0; i < writeVectorCount; ++i) {
                        uint32_t decoded[4];

                        if (vl == 2u) {
                            if (zeroExtend) {
                                if (vn == 0u) {
                                    uint32_t s = srcPtr[0];
                                    decoded[0] = s; decoded[1] = s; decoded[2] = s; decoded[3] = s;
                                } else if (vn == 1u) {
                                    decoded[0] = srcPtr[0]; decoded[1] = srcPtr[1];
                                } else if (vn == 2u) {
                                    decoded[0] = srcPtr[0]; decoded[1] = srcPtr[1]; decoded[2] = srcPtr[2];
                                } else {
                                    decoded[0] = srcPtr[0]; decoded[1] = srcPtr[1]; decoded[2] = srcPtr[2]; decoded[3] = srcPtr[3];
                                }
                            } else {
                                if (vn == 0u) {
                                    uint32_t s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[0])));
                                    decoded[0] = s; decoded[1] = s; decoded[2] = s; decoded[3] = s;
                                } else if (vn == 1u) {
                                    decoded[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[0])));
                                    decoded[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[1])));
                                } else if (vn == 2u) {
                                    decoded[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[0])));
                                    decoded[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[1])));
                                    decoded[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[2])));
                                } else {
                                    decoded[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[0])));
                                    decoded[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[1])));
                                    decoded[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[2])));
                                    decoded[3] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcPtr[3])));
                                }
                            }
                        } else if (vl == 1u) {
                            if (zeroExtend) {
                                if (vn == 0u) {
                                    uint16_t r = 0; std::memcpy(&r, srcPtr, 2);
                                    uint32_t s = static_cast<uint32_t>(r);
                                    decoded[0] = s; decoded[1] = s; decoded[2] = s; decoded[3] = s;
                                } else if (vn == 1u) {
                                    uint16_t r[2]; std::memcpy(r, srcPtr, 4);
                                    decoded[0] = r[0]; decoded[1] = r[1];
                                } else if (vn == 2u) {
                                    uint16_t r[3]; std::memcpy(r, srcPtr, 6);
                                    decoded[0] = r[0]; decoded[1] = r[1]; decoded[2] = r[2];
                                } else {
                                    uint16_t r[4]; std::memcpy(r, srcPtr, 8);
                                    decoded[0] = r[0]; decoded[1] = r[1]; decoded[2] = r[2]; decoded[3] = r[3];
                                }
                            } else {
                                if (vn == 0u) {
                                    int16_t r = 0; std::memcpy(&r, srcPtr, 2);
                                    uint32_t s = static_cast<uint32_t>(static_cast<int32_t>(r));
                                    decoded[0] = s; decoded[1] = s; decoded[2] = s; decoded[3] = s;
                                } else if (vn == 1u) {
                                    int16_t r[2]; std::memcpy(r, srcPtr, 4);
                                    decoded[0] = static_cast<uint32_t>(static_cast<int32_t>(r[0]));
                                    decoded[1] = static_cast<uint32_t>(static_cast<int32_t>(r[1]));
                                } else if (vn == 2u) {
                                    int16_t r[3]; std::memcpy(r, srcPtr, 6);
                                    decoded[0] = static_cast<uint32_t>(static_cast<int32_t>(r[0]));
                                    decoded[1] = static_cast<uint32_t>(static_cast<int32_t>(r[1]));
                                    decoded[2] = static_cast<uint32_t>(static_cast<int32_t>(r[2]));
                                } else {
                                    int16_t r[4]; std::memcpy(r, srcPtr, 8);
                                    decoded[0] = static_cast<uint32_t>(static_cast<int32_t>(r[0]));
                                    decoded[1] = static_cast<uint32_t>(static_cast<int32_t>(r[1]));
                                    decoded[2] = static_cast<uint32_t>(static_cast<int32_t>(r[2]));
                                    decoded[3] = static_cast<uint32_t>(static_cast<int32_t>(r[3]));
                                }
                            }
                        } else { // vl == 0u
                            if (vn == 0u) {
                                uint32_t s = 0; std::memcpy(&s, srcPtr, 4);
                                decoded[0] = s; decoded[1] = s; decoded[2] = s; decoded[3] = s;
                            } else if (vn == 1u) {
                                std::memcpy(&decoded[0], srcPtr, 8);
                            } else if (vn == 2u) {
                                std::memcpy(&decoded[0], srcPtr, 12);
                            } else {
                                std::memcpy(&decoded[0], srcPtr, 16);
                            }
                        }

                        const uint32_t c = (cyclePos > 3u) ? 3u : cyclePos;
                        uint32_t *d = reinterpret_cast<uint32_t*>(targetVuData) + (destVec * 4u);

                        if (act[c][0] == ACT_DECODED) d[0] = decoded[0]; else if (act[c][0] == ACT_CONST) d[0] = constVal[c][0];
                        if (act[c][1] == ACT_DECODED) d[1] = decoded[1]; else if (act[c][1] == ACT_CONST) d[1] = constVal[c][1];
                        if (act[c][2] == ACT_DECODED) d[2] = decoded[2]; else if (act[c][2] == ACT_CONST) d[2] = constVal[c][2];
                        if (act[c][3] == ACT_DECODED) d[3] = decoded[3]; else if (act[c][3] == ACT_CONST) d[3] = constVal[c][3];

                        srcPtr += bytesPerVector;
                        cyclePos++;
                        destVec++;
                        if (cyclePos == wl) {
                            cyclePos = 0;
                            destVec += (cl - wl);
                        }
                    }
                };

                const bool verifyActive = isUnpackVerifyEnabled();
                const bool fastActive = isUnpackFastEnabled();

                bool executeGeneric = true;
                std::vector<uint8_t> savedBefore;
                std::vector<uint8_t> fastResult;
                size_t verifyRangeBytes = 0;
                uint8_t *verifyRangeBase = nullptr;

                if (canFastPath && fastActive) {
                    if (vpAsync) {
                        runFastPath(s_vpScratch);
                        if (!allProtect) {
                            const uint32_t firstRecIndex = vpRecTail.load(std::memory_order_seq_cst);
                            uint32_t recCount = 0;
                            for (uint32_t i = 0u; i < writeVectorCount; ++i) {
                                const uint32_t destVec = vuAddr + (i / wl) * cl + (i % wl);
                                const uint32_t c = std::min(i % wl, 3u);
                                uint8_t mask = 0;
                                for (uint32_t f = 0u; f < 4u; ++f) {
                                    if (act[c][f] != ACT_PROTECT)
                                        mask |= static_cast<uint8_t>(1u << f);
                                }
                                if (mask != 0) {
                                    VpRec rec;
                                    rec.qw = static_cast<uint16_t>(destVec);
                                    rec.mask = mask;
                                    rec.pad = 0;
                                    std::memcpy(rec.lanes, s_vpScratch + destVec * 16u, 16);
                                    vpPushRec(rec);
                                    ++recCount;
                                }
                            }
                            if (recCount > 0) {
                                vpQueueWrites(firstRecIndex, recCount); // PERF-V1H
                            }
                        }
                        if (s_unpackCountsOn)
                        {
                            s_ciFastCount.fetch_add(1, std::memory_order_relaxed);
                            s_dbFastVecs.fetch_add(writeVectorCount, std::memory_order_relaxed);
                        }
                        executeGeneric = false;
                    } else if (verifyActive) {
                        const uint32_t minVec = vuAddr;
                        const uint32_t maxVec = static_cast<uint32_t>(lastVec);
                        verifyRangeBytes = (maxVec - minVec + 1u) * 16u;
                        verifyRangeBase = m_vu1Data + minVec * 16u;

                        savedBefore.assign(verifyRangeBase, verifyRangeBase + verifyRangeBytes);
                        fastResult.resize(verifyRangeBytes);

                        runFastPath(m_vu1Data);
                        std::memcpy(fastResult.data(), verifyRangeBase, verifyRangeBytes);
                        std::memcpy(verifyRangeBase, savedBefore.data(), verifyRangeBytes);

                        executeGeneric = true;
                    } else {
                        runFastPath(m_vu1Data);
                        if (s_unpackCountsOn)
                        {
                            s_ciFastCount.fetch_add(1, std::memory_order_relaxed);
                            s_dbFastVecs.fetch_add(writeVectorCount, std::memory_order_relaxed);
                        }
                        executeGeneric = false;
                        if (probe_ae::isDcProbeEnabled() || isVu3dProbeEnabled()) {
                            uint32_t srcOff = (m_rdram && srcBase >= m_rdram && srcBase < m_rdram + PS2_RAM_SIZE) ? static_cast<uint32_t>(srcBase - m_rdram) : 0u;
                            ps2_vif1_recordDzUnpack(opcode, static_cast<uint8_t>(vn), static_cast<uint8_t>(vl),
                                                    static_cast<uint8_t>((imm & 0x4000u) ? 1u : 0u),
                                                    static_cast<uint8_t>((imm & 0x8000u) ? 1u : 0u),
                                                    static_cast<uint8_t>(cl), static_cast<uint8_t>(wl),
                                                    static_cast<uint8_t>(num), vuAddr, srcOff, 1u);
                        }
                    }
                } else {
                    if (s_unpackCountsOn)
                    {
                        s_ciGenericCount.fetch_add(1, std::memory_order_relaxed);
                        s_dbGenericVecs.fetch_add(writeVectorCount, std::memory_order_relaxed);
                    }
                    if (probe_ae::isEnabled()) {
                        DbShapeKey key{static_cast<uint8_t>(vn), static_cast<uint8_t>(vl),
                                       static_cast<uint8_t>(cl), static_cast<uint8_t>(wl),
                                       static_cast<uint8_t>(maskEnable ? 1u : 0u),
                                       static_cast<uint8_t>(vif1_regs.mode & 3u),
                                       static_cast<uint8_t>((imm & 0x8000u) ? 1u : 0u),
                                       static_cast<uint8_t>((imm & 0x4000u) ? 1u : 0u),
                                       static_cast<uint8_t>(num), static_cast<uint16_t>(vuAddr)};
                        auto &entry = s_dbGenericShapes[key];
                        entry.vecs += writeVectorCount;
                        entry.calls++;
                    }
                }
                if (executeGeneric && (probe_ae::isDcProbeEnabled() || isVu3dProbeEnabled())) {
                    uint32_t srcOff = (m_rdram && srcBase >= m_rdram && srcBase < m_rdram + PS2_RAM_SIZE) ? static_cast<uint32_t>(srcBase - m_rdram) : 0u;
                    ps2_vif1_recordDzUnpack(opcode, static_cast<uint8_t>(vn), static_cast<uint8_t>(vl),
                                            static_cast<uint8_t>((imm & 0x4000u) ? 1u : 0u),
                                            static_cast<uint8_t>((imm & 0x8000u) ? 1u : 0u),
                                            static_cast<uint8_t>(cl), static_cast<uint8_t>(wl),
                                            static_cast<uint8_t>(num), vuAddr, srcOff, 0u);
                }

                if (executeGeneric)
                {
                    const uint32_t firstRecIndex = vpRecTail.load(std::memory_order_seq_cst);
                    uint32_t recCount = 0;
                    uint8_t *vuMem = vpAsync ? s_vpScratch : m_vu1Data;
                    uint32_t srcIndex = 0u;
                    for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                    {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, vuMem + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    if (!handledFormat && decoded) {
                        if (isVu3dProbeEnabled() && vn < 4u && vl < 4u) {
                            uint32_t u = (imm & 0x4000u) ? 1u : 0u;
                            s_ehHandledFalse[vn][vl][u].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(vuMem + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    uint8_t laneMask = 0;
                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                                laneMask |= static_cast<uint8_t>(1u << field);
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                            laneMask |= static_cast<uint8_t>(1u << field);
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                            laneMask |= static_cast<uint8_t>(1u << field);
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    if (vpAsync)
                    {
                        std::memcpy(s_vpScratch + destOff, lanes, sizeof(lanes));
                        if (laneMask != 0)
                        {
                            VpRec rec;
                            rec.qw = static_cast<uint16_t>(destVec);
                            rec.mask = laneMask;
                            rec.pad = 0;
                            std::memcpy(rec.lanes, lanes, sizeof(lanes));
                            vpPushRec(rec);
                            ++recCount;
                        }
                    }
                    else
                    {
                        std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                    }
                }

                if (vpAsync && recCount > 0)
                {
                    vpQueueWrites(firstRecIndex, recCount); // PERF-V1H
                }

                if (canFastPath && fastActive && verifyActive) {
                    s_ciFastCount.fetch_add(1, std::memory_order_relaxed);
                    s_dbFastVecs.fetch_add(writeVectorCount, std::memory_order_relaxed);
                    if (std::memcmp(fastResult.data(), verifyRangeBase, verifyRangeBytes) != 0) {
                        s_ciMismCount.fetch_add(1, std::memory_order_relaxed);
                        uint32_t logged = s_ciMismLogged.fetch_add(1, std::memory_order_relaxed);
                        if (logged < 10) {
                            char buf[256];
                            std::snprintf(buf, sizeof(buf), "[probe] ci unpack mismatch fmt=0x%02X num=%u vuAddr=0x%03X cl=%u wl=%u\n",
                                          opcode & 0x0Fu, num, vuAddr, cl, wl);
                            ciLogMsg(buf);
                        }
                    }
                }
                if (isVu3dProbeEnabled()) {
                    if (vn < 4u && vl < 4u) {
                        uint32_t u = (imm & 0x4000u) ? 1u : 0u;
                        s_ehShapeSeen[vn][vl][u].store(true, std::memory_order_relaxed);
                        if (!executeGeneric) {
                            s_ehFastUsed[vn][vl][u].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (vuAddr <= 720u && lastVec >= 440u) {
                        uint8_t s0[16]{};
                        uint8_t s1[16]{};
                        size_t copy0 = std::min<size_t>(bytesPerVector, 16u);
                        if (pos + copy0 <= sizeBytes) std::memcpy(s0, srcBase, copy0);
                        if (sourceVectorCount >= 2u) {
                            size_t copy1 = std::min<size_t>(bytesPerVector, 16u);
                            if (pos + bytesPerVector + copy1 <= sizeBytes) std::memcpy(s1, srcBase + bytesPerVector, copy1);
                        }
                        uint32_t d0[4]{};
                        if (vuAddr < 0x400u && (vuAddr + 1u) * 16u <= PS2_VU1_DATA_SIZE) {
                            std::memcpy(d0, m_vu1Data + vuAddr * 16u, 16);
                        }
                        uint32_t srcOff = (m_rdram && srcBase >= m_rdram && srcBase < m_rdram + PS2_RAM_SIZE) ? static_cast<uint32_t>(srcBase - m_rdram) : 0u;
                        ps2_vif1_recordEhUnpack(opcode, static_cast<uint8_t>(vn), static_cast<uint8_t>(vl),
                                                static_cast<uint8_t>((imm & 0x4000u) ? 1u : 0u),
                                                static_cast<uint8_t>((imm & 0x8000u) ? 1u : 0u),
                                                static_cast<uint8_t>(cl), static_cast<uint8_t>(wl),
                                                static_cast<uint8_t>(num), vuAddr, srcOff, !executeGeneric ? 1u : 0u,
                                                s0, s1, d0,
                                                vif1_regs.mask, vif1_regs.row, vif1_regs.col);
                    }
                }
                if (m_vu1Data && isVu3dProbeEnabled()) // PERF-UV
                {
                    uint32_t eff = vuAddr;
                    uint32_t lastVec = (cl >= wl) ? ((eff + ((writeVectorCount - 1u) / wl) * cl + ((writeVectorCount - 1u) % wl)) & 0x3FFu) : ((eff + writeVectorCount - 1u) & 0x3FFu);
                    uint32_t dwFirst[4]{}, dwLast[4]{};
                    if (eff < 0x400u) std::memcpy(dwFirst, m_vu1Data + eff * 16u, 16);
                    if (lastVec < 0x400u) std::memcpy(dwLast, m_vu1Data + lastVec * 16u, 16);
                    recordEpEvent("UNPACK-RB eff=%u first_written_qw=%08X %08X %08X %08X last_qw=%08X %08X %08X %08X",
                                  eff, dwFirst[0], dwFirst[1], dwFirst[2], dwFirst[3],
                                  dwLast[0], dwLast[1], dwLast[2], dwLast[3]);
                }
                if (isVu3dProbeEnabled())
                {
                    ps2_scanCameraRegion("unpack", 0, 0);

                    uint32_t srcOff = (srcRdram != 0xFFFFFFFFu) ? srcRdram : 0u;
                    static std::atomic<uint32_t> s_feUnpackEv{0};
                    static std::atomic<uint32_t> s_feUnpackLogCount{0};
                    uint32_t evNum = s_feUnpackEv.fetch_add(1, std::memory_order_relaxed) + 1u;
                    uint32_t nonzeroQw = 0;
                    bool overlaps600_780 = false;
                    for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex) {
                        uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + (writeIndex % wl)) & 0x3FFu)
                                                      : ((vuAddr + writeIndex) & 0x3FFu);
                        ps2_fe_recordUnpack(destVec, evNum, opcode, static_cast<uint16_t>(vuAddr),
                                            static_cast<uint16_t>(writeVectorCount),
                                            static_cast<uint8_t>(cl), static_cast<uint8_t>(wl),
                                            static_cast<uint8_t>((imm & 0x8000u) ? 1u : 0u),
                                            static_cast<uint16_t>(vif1_regs.tops & 0x3FFu), srcOff);
                        if ((destVec >= 600u && destVec <= 780u) || (destVec >= 440u && destVec <= 520u)) {
                            overlaps600_780 = true;
                        }
                        if (m_vu1Data && (destVec + 1u) * 16u <= PS2_VU1_DATA_SIZE) {
                            uint32_t qwW[4];
                            std::memcpy(qwW, m_vu1Data + destVec * 16u, 16);
                            if (qwW[0] != 0u || qwW[1] != 0u || qwW[2] != 0u || qwW[3] != 0u) {
                                nonzeroQw++;
                            }
                        }
                    }

                    if (overlaps600_780 && !(vuAddr == 601u && writeVectorCount == 4u) && s_feUnpackLogCount.fetch_add(1, std::memory_order_relaxed) < 400u)
                    {
                        uint32_t dwFirst[4]{}, dwLast[4]{};
                        if (m_vu1Data) {
                            uint32_t eff = vuAddr & 0x3FFu;
                            uint32_t lastVec = (cl >= wl) ? ((vuAddr + ((writeVectorCount - 1u) / wl) * cl + ((writeVectorCount - 1u) % wl)) & 0x3FFu)
                                                          : ((vuAddr + writeVectorCount - 1u) & 0x3FFu);
                            if ((eff + 1u) * 16u <= PS2_VU1_DATA_SIZE) std::memcpy(dwFirst, m_vu1Data + eff * 16u, 16);
                            if ((lastVec + 1u) * 16u <= PS2_VU1_DATA_SIZE) std::memcpy(dwLast, m_vu1Data + lastVec * 16u, 16);
                        }
                        char srcBuf[32];
                        if (srcOff != 0u) {
                            std::snprintf(srcBuf, sizeof(srcBuf), "src=0x%08X", srcOff);
                        } else {
                            std::snprintf(srcBuf, sizeof(srcBuf), "src=?");
                        }
                        std::printf("[probe] fe unpack ev=%u cmd=0x%02X dest=%u num=%u cl=%u wl=%u flg=%u tops=%u %s nonzero_qw=%u/%u first_qw=%08X_%08X_%08X_%08X last_qw=%08X_%08X_%08X_%08X\n",
                                    evNum, opcode, vuAddr, writeVectorCount, cl, wl,
                                    (imm & 0x8000u) ? 1u : 0u, vif1_regs.tops & 0x3FFu,
                                    srcBuf, nonzeroQw, writeVectorCount,
                                    dwFirst[0], dwFirst[1], dwFirst[2], dwFirst[3],
                                    dwLast[0], dwLast[1], dwLast[2], dwLast[3]);
                        std::fflush(stdout);
                    }
                }
                if (opcode == 0x6Cu && writeVectorCount == 10u && isVu3dProbeEnabled())
                {
                    if (vuAddr == 235u)
                    {
                        std::lock_guard<std::mutex> lock(s_evMutex);
                        s_evEffDest = vuAddr;
                        s_evPathFast = !executeGeneric;
                        s_evSrcRdram = (m_rdram && srcBase >= m_rdram && srcBase < m_rdram + PS2_RAM_SIZE) ? static_cast<uint32_t>(srcBase - m_rdram) : 0xFFFFFFFFu;
                        for (uint32_t i = 0; i < 10u; ++i)
                        {
                            if (pos + (i + 1u) * 16u <= sizeBytes)
                            {
                                std::memcpy(s_evSrc10[i], srcBase + i * 16u, 16);
                            }
                            else
                            {
                                std::memset(s_evSrc10[i], 0, 16);
                            }
                            if (m_vu1Data)
                            {
                                std::memcpy(s_evDst10[i], m_vu1Data + (235u + i) * 16u, 16);
                            }
                        }
                        s_evN235.fetch_add(1, std::memory_order_relaxed);
                        s_euUnpackCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            static thread_local uint32_t s_ciClkTick = 0; // PERF-CT: read the clock once per 256 unpacks, not on every one
            auto now = ((++s_ciClkTick & 255u) == 0u) ? std::chrono::steady_clock::now() : s_ciLastSecLog;
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_ciLastSecLog).count() >= 1) {
                s_ciLastSecLog = now;
                char buf[256];
                std::snprintf(buf, sizeof(buf), "[probe] ci unpack fast=%llu generic=%llu mism=%llu\n",
                              (unsigned long long)s_ciFastCount.load(std::memory_order_relaxed),
                              (unsigned long long)s_ciGenericCount.load(std::memory_order_relaxed),
                              (unsigned long long)s_ciMismCount.load(std::memory_order_relaxed));
                ciLogMsg(buf);

                if (probe_ae::isEnabled()) {
                    std::vector<std::pair<DbShapeKey, DbShapeStats>> sortedShapes(s_dbGenericShapes.begin(), s_dbGenericShapes.end());
                    std::sort(sortedShapes.begin(), sortedShapes.end(), [](const auto &a, const auto &b) {
                        if (a.second.vecs != b.second.vecs) return a.second.vecs > b.second.vecs;
                        return a.second.calls > b.second.calls;
                    });

                    const size_t printCount = std::min<size_t>(6u, sortedShapes.size());
                    for (size_t i = 0; i < printCount; ++i) {
                        const auto &k = sortedShapes[i].first;
                        const auto &s = sortedShapes[i].second;
                        char shBuf[256];
                        std::snprintf(shBuf, sizeof(shBuf),
                            "[probe] db unpack generic shape vn=%u vl=%u cl=%u wl=%u mask=%u mode=%u flg=%u usn=%u num=%u vuAddr=0x%03X vecs=%llu calls=%u\n",
                            k.vn, k.vl, k.cl, k.wl, k.mask, k.mode, k.flg, k.usn, k.num, k.vuAddr,
                            (unsigned long long)s.vecs, s.calls);
                        ciLogMsg(shBuf);
                    }

                    uint64_t fastV = s_dbFastVecs.exchange(0, std::memory_order_relaxed);
                    uint64_t genV = s_dbGenericVecs.exchange(0, std::memory_order_relaxed);
                    char totBuf[256];
                    std::snprintf(totBuf, sizeof(totBuf), "[probe] db unpack fast_vecs=%llu generic_vecs=%llu\n",
                                  (unsigned long long)fastV, (unsigned long long)genV);
                    ciLogMsg(totBuf);

                    s_dbGenericShapes.clear();
                }
            }
            }
            else
            {
                if (isVu3dProbeEnabled())
                {
                    s_erDropped[opcode & 0x7Fu].fetch_add(1, std::memory_order_relaxed);
                }
            }
            pos += totalBytes;

            if (vpOn)
            {
                ps2_gs_async_batch(false);
                ps2_gs_async_batch(true);
            }

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onVifUnknown(opcode, imm);
            }
#endif
            continue;
        }
    }

#if defined(PS2X_PROBE_AE) || 1
    if (probe_ae::isEnabled()) {
        probe_ae::onVif1DmaEnd();
        probe_ae::onAwVifKickEnd(m_rdram);
        auto tVif1End = std::chrono::steady_clock::now();
        probe_ae::onAyTimeVif1(std::chrono::duration_cast<std::chrono::microseconds>(tVif1End - tVif1Start).count());
    }
#endif
    if (kickUnpacksSkipped > 0) {
        probe_ae::recordBlSkipUnpack(kickUnpacksSkipped, kickBytesSkipped);
    }
    probe_ae::onBhVif1End();
    probe_ae::flushMovieBatch(nullptr, this);
}
