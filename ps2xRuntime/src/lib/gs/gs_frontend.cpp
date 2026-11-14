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
    for (int i = 0; i < cN; ++i)
        s_T[i] = 0;
    s_prims = s_imgBytes = s_runs = s_brk = s_redund = 0;
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
            return;
        }
        request = buildPresentationRequestUnlocked();
    }

    PresentationFrame frame{};
    const uint64_t pjStart = __rdtsc(); // PERF-JD
    struct GwPres { uint64_t t0; ~GwPres() { if (g_gwOn) { g_gwPresentT.fetch_add(__rdtsc() - t0, std::memory_order_relaxed); g_gwFrames.fetch_add(1, std::memory_order_relaxed); } } } gwPres{pjStart}; // PROBE-GW
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            const uint64_t pjMid = __rdtsc();
            frame = m_backend->Present(request);
            g_presTicks[1].fetch_add(pjMid - pjStart, std::memory_order_relaxed);
            g_presTicks[2].fetch_add(__rdtsc() - pjMid, std::memory_order_relaxed);
        }
    }

    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
        g_hostPresentSeq.fetch_add(1, std::memory_order_release);
        ft2OnPresent(); // PERF-LN
        if (g_ps2Ft2On) ps2_ft2_ev(12, g_hostPresentSeq.load(std::memory_order_relaxed)); // PROBE-FT2: GS_FRAME_DONE
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

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        const uint32_t gifStatDataOffset = offset;
        const uint64_t gifStatTsc0 = s_gifStatOn ? __rdtsc() : 0ull;

        if (flg == GIF_FMT_PACKED)
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
        else if (flg == GIF_FMT_REGLIST)
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
        else if (flg == GIF_FMT_IMAGE)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
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
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
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
    if (g_gwOn) ps2_gw_count(1, sizeBytes); // PROBE-GW
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

void GS::queueDraw(const GSPrimitiveBatch &batch)
{
    GwScope gwS(5); // PROBE-GW primbuild
    if (g_gwOn) ps2_gw_count(0, 1); // PROBE-GW prims
    if (!m_runPrims.empty() && m_runSerial != m_stateSerial)
    {
        if (g_gwOn) ps2_gw_count(3, 1); // PROBE-GW state-serial break
        flushPendingDraws();
    }
    if (m_runPrims.empty())
    {
        m_runState = batch.state;
        m_runSerial = m_stateSerial;
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
