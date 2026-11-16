#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"
#include "probe_ae.h"
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>
// PERF-KL: cached pointer to the GIF_STAT entry of m_ioRegisters (advanceEeTimers cleared its FQC bits with a hash lookup on every cycle-accounting call)
static uint32_t *s_gifStatPtr = nullptr;
static const void *s_gifStatOwner = nullptr;
void ps2xSafeLog(const char *fmt, ...);

std::atomic<uint64_t> g_gsFrameKicks{0};
std::atomic<uint64_t> g_aw2Mark2000{0}, g_aw2C5Raised{0}, g_aw2C5Delivered{0}; // PROBE-AW2
extern std::atomic<uint32_t> g_aw3LastVifCode; // PROBE-AW3
extern std::atomic<bool> g_aw3SawMark2000; // PROBE-AW3

static inline bool aw3ProbeOn() // PROBE-AW3
{ // PROBE-AW3
    static const bool s_on = []() { const char *e = std::getenv("PS2X_AW2_PROBE"); return e && e[0] == '1'; }(); // PROBE-AW3
    return s_on; // PROBE-AW3
} // PROBE-AW3

struct Aw3ChainEntry // PROBE-AW3
{ // PROBE-AW3
    char walker = '?'; // PROBE-AW3
    uint32_t tadr0 = 0; // PROBE-AW3
    uint32_t tags = 0; // PROBE-AW3
    const char *endReason = "other"; // PROBE-AW3
    uint32_t tadrEnd = 0; // PROBE-AW3
    uint32_t chcr = 0; // PROBE-AW3
    int mark2000 = 0; // PROBE-AW3
    uint32_t lastvc = 0; // PROBE-AW3
}; // PROBE-AW3

static std::mutex g_aw3RingMutex; // PROBE-AW3
static Aw3ChainEntry g_aw3Ring[16]; // PROBE-AW3
static uint32_t g_aw3RingWrite = 0; // PROBE-AW3
static uint32_t g_aw3RingCount = 0; // PROBE-AW3

static void aw3RecordChain(char walker, uint32_t tadr0, uint32_t tags, const char *endReason, uint32_t tadrEnd, uint32_t chcr) // PROBE-AW3
{ // PROBE-AW3
    std::lock_guard<std::mutex> lock(g_aw3RingMutex); // PROBE-AW3
    Aw3ChainEntry &e = g_aw3Ring[g_aw3RingWrite & 15u]; // PROBE-AW3
    e.walker = walker; // PROBE-AW3
    e.tadr0 = tadr0; // PROBE-AW3
    e.tags = tags; // PROBE-AW3
    e.endReason = endReason; // PROBE-AW3
    e.tadrEnd = tadrEnd; // PROBE-AW3
    e.chcr = chcr; // PROBE-AW3
    e.mark2000 = g_aw3SawMark2000.load(std::memory_order_relaxed) ? 1 : 0; // PROBE-AW3
    e.lastvc = g_aw3LastVifCode.load(std::memory_order_relaxed); // PROBE-AW3
    ++g_aw3RingWrite; // PROBE-AW3
    if (g_aw3RingCount < 16u) ++g_aw3RingCount; // PROBE-AW3
} // PROBE-AW3

void aw3DumpRing() // PROBE-AW3
{ // PROBE-AW3
    std::lock_guard<std::mutex> lock(g_aw3RingMutex); // PROBE-AW3
    const uint32_t n = g_aw3RingCount; // PROBE-AW3
    const uint32_t start = (g_aw3RingWrite >= n) ? (g_aw3RingWrite - n) : 0u; // PROBE-AW3
    for (uint32_t i = 0; i < n; ++i) // PROBE-AW3
    { // PROBE-AW3
        const Aw3ChainEntry &e = g_aw3Ring[(start + i) & 15u]; // PROBE-AW3
        ps2xSafeLog("[aw3] #%u walker=%c tadr0=0x%08x tags=%u end=%s tadrEnd=0x%08x chcr=0x%08x mark2000=%d lastvc=0x%08x\n", // PROBE-AW3
                    i, e.walker, e.tadr0, e.tags, e.endReason, e.tadrEnd, e.chcr, e.mark2000, e.lastvc); // PROBE-AW3
    } // PROBE-AW3
} // PROBE-AW3

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }


    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();
        s_gifStatPtr = nullptr; // PERF-KL: the cached element is gone

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

// Task FK2: map VIF1 chain-buffer offsets to guest source addresses (probe only)
#include <mutex>
#include <map>
#include <cstdlib>
#include <chrono>
#include <intrin.h>
// PROBE-KF: wait/busy ticks: 0 vif1 fence wait, 1 vif1 enqueue wait, 2 gs fence wait, 3 gs backpressure wait, 4 vif1 worker busy, 5 fence waits (count)
std::atomic<uint64_t> g_waitTicks[8]{};
extern "C" uint64_t ps2x_ft2_waitTicks(int idx) { return (idx >= 0 && idx < 8) ? g_waitTicks[idx].load(std::memory_order_relaxed) : 0u; } // PERF-LN
extern "C" void ps2x_sched_apply(int flags); // PERF-LU: probe_prof.cpp
extern "C" void ps2_gs_async_fence(); // PERF-GK (gs/ps2_gif_arbiter.cpp)

// PERF-HN: async VIF1/VU1 worker (opt-in PS2X_VIF1_ASYNC=1). The EE thread copies VIF1 packet data into jobs; one worker
// runs processVIF1Data (VIF1 regs, VU1 code/data, VU1 execution, PATH1/PATH2 GIF submits). Every EE-thread access to that
// state fences first (ps2_vif1_fence), so the worker is the exclusive owner while it runs.
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include "ThreadNaming.h" // PERF-MB: thread names for tools/cpu_per_frame.ps1
extern std::function<uint32_t()> g_vif1FbrstProvider;   // ps2_runtime.cpp: EE-thread FBRST (VU0 control reg) snapshot
extern std::function<void()> g_vif1ApplyVpuStat;        // ps2_runtime.cpp: merge worker-produced VPU_STAT bits into the EE context
extern thread_local uint32_t g_vif1JobFbrst;            // ps2_runtime.cpp: FBRST for the job being run by the worker (~0u = none)
namespace
{
    struct DmaChainWalkResult // PERF-GC
    {
        uint32_t tagAddr = 0u;
        uint32_t asr0 = 0u;
        uint32_t asr1 = 0u;
        uint32_t chcr = 0u;
        uint32_t tags = 0u;  // PERF-GG
        uint32_t spans = 0u; // PERF-GG
    };
    struct Vif1Job
    {
        std::vector<uint8_t> data;
        uint32_t fbrst = 0u;
        bool isSourceChain = false; // PERF-GC
        uint32_t initialTadr = 0u;  // PERF-GC
        uint32_t initialChcr = 0u;  // PERF-GC
        uint32_t initialAsr0 = 0u;  // PERF-GC
        uint32_t initialAsr1 = 0u;  // PERF-GC
        uint32_t initialMadr = 0u;  // PERF-GC
        uint32_t initialQwc = 0u;   // PERF-GC
        uint64_t copySeq = 0u;      // PERF-GC
    };
    struct Vif1AsyncState
    {
        std::mutex m;
        std::condition_variable cvWork, cvIdle, cvSpace;
        std::deque<Vif1Job> q;
        size_t queuedBytes = 0;
        bool busy = false;
        bool started = false;
        std::atomic<bool> pending{false}; // queue non-empty or worker busy
        uint64_t enqueuedJobs = 0, doneJobs = 0; // PERF-SL: job sequence numbers (guarded by m)
        uint32_t lagWaiters = 0;                 // PERF-SL: EE threads waiting in ps2_vif1_sync_lag
        std::atomic<uint64_t> copyEnqueuedSeq{0}; // PERF-GC
        std::atomic<uint64_t> copyDoneSeq{0};     // PERF-GC
        std::mutex copyDoneMtx;                   // PERF-GC
        std::condition_variable copyDoneCv;       // PERF-GC
        uint32_t pubTadr = 0u;                    // PERF-GC
        uint32_t pubMadr = 0u;                    // PERF-GC
        uint32_t pubQwc = 0u;                     // PERF-GC
        uint32_t pubChcr = 0u;                    // PERF-GC
        uint32_t pubAsr0 = 0u;                    // PERF-GC
        uint32_t pubAsr1 = 0u;                    // PERF-GC
        std::atomic<bool> pubSyncPending{false};  // PERF-GC
        std::atomic<bool> pubDStatPending{false}; // PERF-GC
        std::deque<Vif1Job> copierQ;              // PERF-GF
        std::condition_variable cvCopierWork;     // PERF-GF
        bool copierBusy = false;                  // PERF-GF
        bool copierStarted = false;               // PERF-GF
        std::atomic<uint64_t> copierEeWaitTicks{0}; // PERF-GF
        std::atomic<uint64_t> copierPrescanTicks{0};  // PERF-GG
        std::atomic<uint64_t> copierPrescanChains{0}; // PERF-GG
    };
    Vif1AsyncState &vifAsync()
    {
        static Vif1AsyncState *s = new Vif1AsyncState();
        return *s;
    }
    bool vif1AsyncEnabled()
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_VIF1_ASYNC"); return e && e[0] == '1'; }();
        return on;
    }
    bool dma1WorkerCopyEnabled() // PERF-GC
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_DMA1_WORKER_COPY"); return !(e && e[0] == '0'); }();
        return on;
    }
    bool dma1CopierEnabled() // PERF-GF
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_DMA1_COPIER"); return e && e[0] == '1'; }();
        return on;
    }
    bool dma1CopierActive() // PERF-GF
    {
        return dma1CopierEnabled() && dma1WorkerCopyEnabled() && vif1AsyncEnabled();
    }
    bool markNoFenceEnabled() // PERF-V1K
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_MARK_NOFENCE"); return !(e && e[0] == '0'); }();
        return on;
    }
    bool ggWalkEnabled() // PERF-GG
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_GG"); return !(e && e[0] == '0'); }();
        return on;
    }
    bool ggCheckEnabled() // PERF-GG
    {
        static const bool on = []() { const char *e = std::getenv("PS2X_GG_CHECK"); return e && e[0] == '1'; }();
        return on;
    }
    constexpr uint32_t kVif1StcClearMask = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13); // PERF-V1K
    thread_local bool t_onVif1Worker = false;
    constexpr size_t kVif1AsyncMaxBytes = 16u * 1024u * 1024u;

    // PERF-VP: recycled DMA-chain buffers. Every EE DMA kick grew a fresh std::vector (repeated realloc + page faults for MB-sized chains) and the
    // worker freed it again (NtFreeVirtualMemory showed up in the sampler). The worker now returns the buffer after processing it.
    struct VifBufPool // leaked on purpose like Vif1AsyncState (detached worker)
    {
        std::mutex m;
        std::vector<std::vector<uint8_t>> v;
    };
    VifBufPool &vifBufPool()
    {
        static VifBufPool *p = new VifBufPool();
        return *p;
    }
    std::vector<uint8_t> takeVifBuf()
    {
        VifBufPool &pool = vifBufPool();
        std::lock_guard<std::mutex> lock(pool.m);
        if (pool.v.empty())
            return {};
        std::vector<uint8_t> b = std::move(pool.v.back());
        pool.v.pop_back();
        b.clear();
        return b;
    }
    void recycleVifBuf(std::vector<uint8_t> &&b)
    {
        if (b.capacity() == 0u || b.capacity() > (32u << 20))
            return;
        VifBufPool &pool = vifBufPool();
        std::lock_guard<std::mutex> lock(pool.m);
        if (pool.v.size() < 8u)
            pool.v.push_back(std::move(b));
    }
}
static std::function<void(const uint8_t *, uint32_t)> s_vif1Process; // bound by a PS2Memory member (private access)
static std::function<void()> s_vif1DrainArbiter;
static std::function<DmaChainWalkResult(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::vector<uint8_t> &)> s_vif1WalkChain; // PERF-GC
static std::function<void(uint32_t)> s_vif1QueueDmacCause; // PERF-GC
static thread_local bool t_vif1ScanOnly = false;   // PERF-GC tag-only pre-scan mode for s_vif1WalkChain
static thread_local bool t_vif1ScanHitSpr = false; // PERF-GC set when scan sees a scratchpad tag/payload
static std::atomic<uint64_t> g_ggMismatches{0};    // PERF-GG
static thread_local uint64_t t_ggLastWalkTicks = 0; // PERF-GG
static void waitDma1CopyDone() // PERF-GC
{
    if (t_onVif1Worker)
        return;
    Vif1AsyncState &st = vifAsync();
    const uint64_t want = st.copyEnqueuedSeq.load(std::memory_order_acquire);
    if (st.copyDoneSeq.load(std::memory_order_acquire) >= want)
        return;
    const bool trackWait = dma1CopierActive(); // PERF-GF
    const uint64_t wT0 = trackWait ? __rdtsc() : 0ull; // PERF-GF
    for (int i = 0; i < 64; ++i)
    {
        if (st.copyDoneSeq.load(std::memory_order_acquire) >= want)
        {
            if (trackWait) st.copierEeWaitTicks.fetch_add(__rdtsc() - wT0, std::memory_order_relaxed); // PERF-GF
            return;
        }
        _mm_pause();
    }
    std::unique_lock<std::mutex> lock(st.copyDoneMtx);
    st.copyDoneCv.wait(lock, [&] { return st.copyDoneSeq.load(std::memory_order_acquire) >= want; });
    if (trackWait) st.copierEeWaitTicks.fetch_add(__rdtsc() - wT0, std::memory_order_relaxed); // PERF-GF
}
static void syncDma1PublishedToIoRegs(std::unordered_map<uint32_t, uint32_t> &ioRegs) // PERF-GC
{
    if (t_onVif1Worker)
        return;
    Vif1AsyncState &st = vifAsync();
    if (!st.pubSyncPending.load(std::memory_order_acquire) && !st.pubDStatPending.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(st.copyDoneMtx);
    if (st.pubSyncPending.exchange(false, std::memory_order_acq_rel))
    {
        static constexpr uint32_t kVif1Base = 0x10009000u;
        ioRegs[kVif1Base + 0x00u] = st.pubChcr;
        ioRegs[kVif1Base + 0x10u] = st.pubMadr;
        ioRegs[kVif1Base + 0x20u] = st.pubQwc;
        ioRegs[kVif1Base + 0x30u] = st.pubTadr;
        ioRegs[kVif1Base + 0x40u] = st.pubAsr0;
        ioRegs[kVif1Base + 0x50u] = st.pubAsr1;
    }
    if (st.pubDStatPending.exchange(false, std::memory_order_acq_rel))
    {
        static constexpr uint32_t kDStat = 0x1000E010u;
        uint32_t dstat = ioRegs.count(kDStat) ? ioRegs[kDStat] : 0u;
        dstat |= (1u << 1u);
        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16u) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31u);
        else
            dstat &= ~(1u << 31u);
        ioRegs[kDStat] = dstat;
    }
}
extern "C" void ps2_gs_async_batch(bool on); // PERF-GB: gs/ps2_gif_arbiter.cpp
extern bool g_ps2Ft2On; extern "C" void ps2_ft2_ev(uint32_t type, uint64_t arg); // PROBE-FT2
// PROBE-TL: job timeline (PS2X_TL=1). Ring indexed by job sequence (enqueue order == worker pop order).
static const bool g_tlOn = []() { const char *e = std::getenv("PS2X_TL"); return e && e[0] == '1'; }(); // PROBE-TL
static std::atomic<uint64_t> g_tlEnq[1024]{};                                                           // PROBE-TL
static std::atomic<uint64_t> g_tlNewest{0};                                                             // PROBE-TL: tsc of newest enqueue
static std::atomic<uint64_t> g_tlJobs{0}, g_tlQueue{0}, g_tlDur{0}, g_tlDurMax{0};                      // PROBE-TL
static std::atomic<uint64_t> g_tlFences{0}, g_tlFWait{0}, g_tlLead{0};                                  // PROBE-TL
static void tlFence(uint64_t fT0, uint64_t fT1)                                                         // PROBE-TL
{
    const uint64_t nw = g_tlNewest.load(std::memory_order_relaxed);
    g_tlFences.fetch_add(1, std::memory_order_relaxed);
    g_tlFWait.fetch_add(fT1 - fT0, std::memory_order_relaxed);
    if (nw && fT0 > nw)
        g_tlLead.fetch_add(fT0 - nw, std::memory_order_relaxed);
}
// PROBE-FT2: PS2X_FT2=1 per-frame event timeline. Starts 20 s after first VBLANK, fills 4096 events, dumps logs\ft2.txt once.
#include <chrono>   // PROBE-FT2
#include <cstdio>   // PROBE-FT2
bool g_ps2Ft2On = []() { const char *e = std::getenv("PS2X_FT2"); return e && e[0] == '1'; }(); // PROBE-FT2
namespace ft2rec // PROBE-FT2
{
constexpr uint32_t kMax = 262144; // PROBE-FT2
static uint32_t kN = []() { const char *e = std::getenv("PS2X_FT2_N"); long v = e ? std::atol(e) : 4096; if (v < 1) v = 4096; if (v > (long)kMax) v = kMax; return (uint32_t)v; }(); // PROBE-FT2
static const double kStartS = []() { const char *e = std::getenv("PS2X_FT2_START_S"); double v = e ? std::atof(e) : 20.0; return v < 0 ? 20.0 : v; }(); // PROBE-FT2
struct Ev { std::atomic<uint64_t> tsc{0}; uint64_t arg = 0; uint32_t type = 0, tid = 0; };
static Ev g_ev[kMax]; // PROBE-FT2
static std::atomic<uint32_t> g_idx{0};
static std::atomic<bool> g_active{false}, g_dumped{false};
static std::atomic<uint32_t> g_tidGen{0};
static uint64_t g_firstTsc = 0;
static std::chrono::steady_clock::time_point g_firstT;
static bool g_haveFirst = false; // touched only by the VBLANK (EE scheduler) thread
static const char *const kNames[] = {"VBLANK", "DMA1_KICK", "JOB_START", "JOB_PARSE_END", "EXEC_LAST", "JOB_DONE", "FENCE_IN", "FENCE_OUT",
                                     "SYNCPATH_IN", "SYNCPATH_OUT", "GIF_SUBMIT_BLOCK_IN", "GIF_SUBMIT_BLOCK_OUT", "GS_FRAME_DONE", "PRESENT", "EEFN", "COPY_DONE"}; // PROBE-FT2 // PERF-GC
static void dump()
{
    if (g_dumped.exchange(true))
        return;
    const uint64_t t1 = __rdtsc();
    const auto s1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(s1 - g_firstT).count();
    const double tpu = us > 0.0 ? static_cast<double>(t1 - g_firstTsc) / us : 1.0; // tsc ticks per microsecond over the recording
    uint64_t base = ~0ull;
    for (uint32_t i = 0; i < kN; ++i)
    {
        const uint64_t t = g_ev[i].tsc.load(std::memory_order_acquire);
        if (t && t < base) base = t;
    }
    FILE *f = std::fopen("logs\\ft2.txt", "w");
    if (!f) return;
    for (uint32_t i = 0; i < kN; ++i)
    {
        const uint64_t t = g_ev[i].tsc.load(std::memory_order_acquire);
        if (!t) continue;
        std::fprintf(f, "%.1f %s %llu T%u\n", static_cast<double>(t - base) / tpu, kNames[g_ev[i].type], static_cast<unsigned long long>(g_ev[i].arg), g_ev[i].tid);
    }
    std::fclose(f);
    g_active.store(false);
}
} // namespace ft2rec
extern "C" uint32_t probe_getNewestDistinctFn(); // PROBE-FT2
extern "C" void ps2_ft2_ev(uint32_t type, uint64_t arg) // PROBE-FT2
{
    using namespace ft2rec;
    if (type == 0) // VBLANK: arms the 20 s delay
    {
        const auto now = std::chrono::steady_clock::now();
        if (!g_haveFirst) { g_haveFirst = true; g_firstT = now; }
        else if (!g_active.load(std::memory_order_relaxed) && !g_dumped.load(std::memory_order_relaxed) &&
                 now - g_firstT >= std::chrono::duration<double>(kStartS))
        {
            g_firstT = now; g_firstTsc = __rdtsc();
            g_active.store(true, std::memory_order_release);
        }
    }
    if (!g_active.load(std::memory_order_acquire))
    {
        if (g_idx.load(std::memory_order_relaxed) >= kN && (type == 0 || type == 13)) dump();
        return;
    }
    const uint32_t i = g_idx.fetch_add(1, std::memory_order_relaxed);
    if (i >= kN) { g_active.store(false); return; }
    static thread_local uint32_t t_tid = ~0u;
    if (t_tid == ~0u) t_tid = g_tidGen.fetch_add(1, std::memory_order_relaxed);
    Ev &e = g_ev[i];
    e.type = type; e.arg = arg; e.tid = t_tid;
    e.tsc.store(__rdtsc(), std::memory_order_release);
    if (type == 0) ps2_ft2_ev(14, probe_getNewestDistinctFn()); // PROBE-FT2: EEFN sampled at every VBLANK
}
extern bool ps2xVwOn(); extern void ps2xVwJob(uint64_t idle, uint64_t outside, uint64_t proc); // PROBE-VW (ps2_vif1_interpreter.cpp)
static void vif1Worker()
{
    t_onVif1Worker = true;
    uint64_t ft2Seq = 0; // PROBE-FT2
    uint64_t tlSeq = 0; // PROBE-TL
    ThreadNaming::SetCurrentThreadName("Vif1Worker");
    { // PERF-LU
        static const bool s_sched = []() { const char *e = std::getenv("PS2X_SCHED"); return e && e[0] == '1'; }();
        if (s_sched)
        {
            ps2x_sched_apply(2);
            ps2xSafeLog("[sched] ON Vif1Worker\n");
            std::fflush(stderr);
        }
    }
    // PROBE-KJ: PS2X_PROF_VU1WORKER=1 (with PS2X_PROF_SAMPLE=1) points the sampler's stack/owner/budget2 output at this worker instead of the EE thread
    if (const char *pe = std::getenv("PS2X_PROF_VU1WORKER"); pe && pe[0] == '1')
        probe_ae::recordMainGuestThread();
    Vif1AsyncState &st = vifAsync();
    const bool vwOn = ps2xVwOn(); // PROBE-VW
    for (;;)
    {
        uint64_t vwIdle = 0, vwW1 = 0; // PROBE-VW
        Vif1Job job;
        {
            std::unique_lock<std::mutex> lock(st.m);
            const uint64_t idT0 = __rdtsc(); // PROBE-KF: worker idle = waiting for the EE to enqueue the next job
            st.cvWork.wait(lock, [&] { return !st.q.empty(); });
            vwIdle = __rdtsc() - idT0; // PROBE-VW
            vwW1 = idT0 + vwIdle; // PROBE-VW
            g_waitTicks[6].fetch_add(vwIdle, std::memory_order_relaxed);
            job = std::move(st.q.front());
            st.q.pop_front();
            st.queuedBytes -= job.data.size();
            st.busy = true;
            st.cvSpace.notify_all();
        }
        g_vif1JobFbrst = job.fbrst;
        const uint64_t tlStart = g_tlOn ? __rdtsc() : 0; // PROBE-TL
        const uint64_t wkT0 = __rdtsc(); // PROBE-KF
        if (g_ps2Ft2On) ps2_ft2_ev(2, ft2Seq); // PROBE-FT2: JOB_START
        std::vector<uint8_t> workBuf; // PERF-GC
        if (job.isSourceChain) // PERF-GC
        {
            workBuf = takeVifBuf();
            const DmaChainWalkResult res = s_vif1WalkChain(0x10009000u, job.initialChcr, job.initialTadr, job.initialAsr0, job.initialAsr1, workBuf);
            {
                std::lock_guard<std::mutex> cLock(st.copyDoneMtx);
                st.pubTadr = res.tagAddr;
                st.pubMadr = job.initialMadr;
                st.pubQwc = workBuf.empty() ? job.initialQwc : 0u;
                st.pubChcr = res.chcr & ~0x100u;
                st.pubAsr0 = res.asr0;
                st.pubAsr1 = res.asr1;
                st.pubDStatPending.store(true, std::memory_order_release);
                st.pubSyncPending.store(true, std::memory_order_release);
            }
            if (s_vif1QueueDmacCause)
                s_vif1QueueDmacCause(1u);
            {
                std::lock_guard<std::mutex> cLock(st.copyDoneMtx);
                st.copyDoneSeq.store(job.copySeq, std::memory_order_release);
            }
            st.copyDoneCv.notify_all();
            if (g_ps2Ft2On) ps2_ft2_ev(15, job.copySeq); // PROBE-FT2: COPY_DONE
        }
        else
        {
            workBuf = std::move(job.data);
        }
        uint64_t vwP0 = 0ull, vwP1 = 0ull; // PROBE-VW
        if (!workBuf.empty()) // PERF-GC
        {
            ps2_gs_async_batch(true); // PERF-GB
            vwP0 = vwOn ? __rdtsc() : 0ull; // PROBE-VW
            s_vif1Process(workBuf.data(), static_cast<uint32_t>(workBuf.size()));
            vwP1 = vwOn ? __rdtsc() : 0ull; // PROBE-VW
            if (g_ps2Ft2On) ps2_ft2_ev(3, ft2Seq++); // PROBE-FT2: JOB_PARSE_END
            ps2_gs_async_batch(false); // PERF-GB: hand the job's GIF packets to the GS queue under one lock
            g_waitTicks[4].fetch_add(__rdtsc() - wkT0, std::memory_order_relaxed);
            if (s_vif1DrainArbiter)
                s_vif1DrainArbiter();
        }
        else
        {
            vwP0 = vwOn ? __rdtsc() : 0ull; // PROBE-VW
            vwP1 = vwP0;
            if (g_ps2Ft2On) ps2_ft2_ev(3, ft2Seq++); // PROBE-FT2: JOB_PARSE_END
            g_waitTicks[4].fetch_add(__rdtsc() - wkT0, std::memory_order_relaxed);
        }
        g_vif1JobFbrst = ~0u;
        if (g_tlOn) // PROBE-TL
        {
            const uint64_t tlEnd = __rdtsc();
            const uint64_t enq = g_tlEnq[tlSeq++ & 1023u].load(std::memory_order_relaxed);
            const uint64_t dur = tlEnd - tlStart;
            g_tlJobs.fetch_add(1, std::memory_order_relaxed);
            if (tlStart > enq)
                g_tlQueue.fetch_add(tlStart - enq, std::memory_order_relaxed);
            g_tlDur.fetch_add(dur, std::memory_order_relaxed);
            uint64_t mx = g_tlDurMax.load(std::memory_order_relaxed);
            while (dur > mx && !g_tlDurMax.compare_exchange_weak(mx, dur, std::memory_order_relaxed)) {}
        }
        recycleVifBuf(std::move(workBuf)); // PERF-VP
        {
            std::lock_guard<std::mutex> lock(st.m);
            st.busy = false;
            ++st.doneJobs; // PERF-SL
            if (g_ps2Ft2On) ps2_ft2_ev(5, st.doneJobs); // PROBE-FT2: JOB_DONE
            if (st.q.empty() && st.copierQ.empty() && !st.copierBusy) // PERF-GF
            {
                st.pending.store(false, std::memory_order_release);
                st.cvIdle.notify_all();
            }
            else if (st.lagWaiters != 0u)
                st.cvIdle.notify_all();
        }
        if (vwOn) ps2xVwJob(vwIdle, (vwP0 - vwW1) + (__rdtsc() - vwP1), vwP1 - vwP0); // PROBE-VW
    }
}
static void vif1PushJobLocked(Vif1AsyncState &st, Vif1Job &&job, bool fromCopier = false, bool wasSourceChain = false) // PERF-GC // PERF-GF
{
    if (!st.started)
    {
        st.started = true;
        std::thread(vif1Worker).detach();
    }
    if (!fromCopier || wasSourceChain) // PERF-GF: non-chain jobs routed via copier were already counted on submit
        st.queuedBytes += job.data.size();
    st.q.push_back(std::move(job));
    if (!fromCopier) // PERF-GF: EE already recorded enqueue seq/tl/ft2 on submit
    {
        if (g_tlOn) // PROBE-TL
        {
            const uint64_t t = __rdtsc();
            g_tlEnq[st.enqueuedJobs & 1023u].store(t, std::memory_order_relaxed);
            g_tlNewest.store(t, std::memory_order_relaxed);
        }
        ++st.enqueuedJobs; // PERF-SL
        if (g_ps2Ft2On) ps2_ft2_ev(1, st.enqueuedJobs); // PROBE-FT2: DMA1_KICK
    }
    st.pending.store(true, std::memory_order_release);
    st.cvWork.notify_one();
}
static void dma1CopierWorker() // PERF-GF
{
    t_onVif1Worker = true;
    t_vif1ScanOnly = false;
    t_vif1ScanHitSpr = false;
    ThreadNaming::SetCurrentThreadName("Dma1Copier");
    Vif1AsyncState &st = vifAsync();
    uint64_t winJobs = 0, winChain = 0, winCopyTicks = 0, winMaxCopyTicks = 0;
    uint64_t winTags = 0, winBytes = 0, winSpans = 0, winWalkTicks = 0; // PERF-GG
    auto winT0 = std::chrono::steady_clock::now();
    uint64_t winTsc0 = __rdtsc();
    for (;;)
    {
        Vif1Job job;
        {
            std::unique_lock<std::mutex> lock(st.m);
            st.cvCopierWork.wait(lock, [&] { return !st.copierQ.empty(); });
            job = std::move(st.copierQ.front());
            st.copierQ.pop_front();
            st.copierBusy = true;
        }
        const bool wasSourceChain = job.isSourceChain;
        if (wasSourceChain)
        {
            std::vector<uint8_t> workBuf = takeVifBuf();
            const uint64_t cT0 = __rdtsc();
            const DmaChainWalkResult res = s_vif1WalkChain(0x10009000u, job.initialChcr, job.initialTadr, job.initialAsr0, job.initialAsr1, workBuf);
            const uint64_t cTicks = __rdtsc() - cT0;
            ++winChain;
            winCopyTicks += cTicks;
            winWalkTicks += t_ggLastWalkTicks; // PERF-GG
            winTags += res.tags;              // PERF-GG
            winBytes += workBuf.size();       // PERF-GG
            winSpans += res.spans;            // PERF-GG
            if (cTicks > winMaxCopyTicks)
                winMaxCopyTicks = cTicks;
            {
                std::lock_guard<std::mutex> cLock(st.copyDoneMtx);
                st.pubTadr = res.tagAddr;
                st.pubMadr = job.initialMadr;
                st.pubQwc = workBuf.empty() ? job.initialQwc : 0u;
                st.pubChcr = res.chcr & ~0x100u;
                st.pubAsr0 = res.asr0;
                st.pubAsr1 = res.asr1;
                st.pubDStatPending.store(true, std::memory_order_release);
                st.pubSyncPending.store(true, std::memory_order_release);
            }
            if (s_vif1QueueDmacCause)
                s_vif1QueueDmacCause(1u);
            {
                std::lock_guard<std::mutex> cLock(st.copyDoneMtx);
                st.copyDoneSeq.store(job.copySeq, std::memory_order_release);
            }
            st.copyDoneCv.notify_all();
            if (g_ps2Ft2On) ps2_ft2_ev(15, job.copySeq); // PROBE-FT2: COPY_DONE

            Vif1Job outJob;
            outJob.fbrst = job.fbrst;
            outJob.data = std::move(workBuf);
            job = std::move(outJob);
        }
        ++winJobs;
        {
            std::lock_guard<std::mutex> lock(st.m);
            st.copierBusy = false;
            vif1PushJobLocked(st, std::move(job), true, wasSourceChain);
        }
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - winT0).count();
        if (ms >= 5000.0)
        {
            const uint64_t tsc = __rdtsc();
            const double tpm = static_cast<double>(tsc - winTsc0) / ms;
            const double tpu = tpm / 1000.0;
            const uint64_t eeTicks = st.copierEeWaitTicks.exchange(0, std::memory_order_relaxed);
            const uint64_t prescanTicks = st.copierPrescanTicks.exchange(0, std::memory_order_relaxed);   // PERF-GG
            const uint64_t prescanChains = st.copierPrescanChains.exchange(0, std::memory_order_relaxed); // PERF-GG
            const double avgUs = (winChain > 0 && tpu > 0.0) ? (static_cast<double>(winCopyTicks) / static_cast<double>(winChain) / tpu) : 0.0;
            const double maxUs = (tpu > 0.0) ? (static_cast<double>(winMaxCopyTicks) / tpu) : 0.0;
            const double eeWaitMs = (tpm > 0.0) ? (static_cast<double>(eeTicks) / tpm) : 0.0;
            const double tagsAvg = (winChain > 0) ? (static_cast<double>(winTags) / static_cast<double>(winChain)) : 0.0; // PERF-GG
            const double bytesAvg = (winChain > 0) ? (static_cast<double>(winBytes) / static_cast<double>(winChain)) : 0.0; // PERF-GG
            const double spansAvg = (winChain > 0) ? (static_cast<double>(winSpans) / static_cast<double>(winChain)) : 0.0; // PERF-GG
            const double prescanUs = (prescanChains > 0 && tpu > 0.0) ? (static_cast<double>(prescanTicks) / static_cast<double>(prescanChains) / tpu) : 0.0; // PERF-GG
            const double walkUs = (winChain > 0 && tpu > 0.0) ? (static_cast<double>(winWalkTicks) / static_cast<double>(winChain) / tpu) : 0.0; // PERF-GG
            ps2xSafeLog("[copier] jobs=%llu chain=%llu copy_us_avg=%.1f max=%.1f ee_wait_ms=%.2f tags=%.1f bytes=%.0f spans=%.1f prescan_us=%.1f walk_us=%.1f gg_mismatch=%llu\n",
                        (unsigned long long)winJobs, (unsigned long long)winChain, avgUs, maxUs, eeWaitMs,
                        tagsAvg, bytesAvg, spansAvg, prescanUs, walkUs,
                        (unsigned long long)g_ggMismatches.load(std::memory_order_relaxed));
            winJobs = 0;
            winChain = 0;
            winCopyTicks = 0;
            winMaxCopyTicks = 0;
            winTags = 0;
            winBytes = 0;
            winSpans = 0;
            winWalkTicks = 0;
            winT0 = now;
            winTsc0 = tsc;
        }
    }
}
static void vif1PushCopierJobLocked(Vif1AsyncState &st, Vif1Job &&job) // PERF-GF
{
    if (!st.copierStarted)
    {
        st.copierStarted = true;
        std::thread(dma1CopierWorker).detach();
    }
    st.queuedBytes += job.data.size();
    st.copierQ.push_back(std::move(job));
    if (g_tlOn) // PROBE-TL
    {
        const uint64_t t = __rdtsc();
        g_tlEnq[st.enqueuedJobs & 1023u].store(t, std::memory_order_relaxed);
        g_tlNewest.store(t, std::memory_order_relaxed);
    }
    ++st.enqueuedJobs; // PERF-SL
    if (g_ps2Ft2On) ps2_ft2_ev(1, st.enqueuedJobs); // PROBE-FT2: DMA1_KICK
    st.pending.store(true, std::memory_order_release);
    st.cvCopierWork.notify_one();
}
static void vif1EnqueueJob(std::vector<uint8_t> &&data)
{
    if (data.empty())
        return;
    Vif1AsyncState &st = vifAsync();
    const uint32_t fbrst = g_vif1FbrstProvider ? g_vif1FbrstProvider() : 0u;
    const bool useCopier = dma1CopierActive(); // PERF-GF
    std::unique_lock<std::mutex> lock(st.m);
    const uint64_t eqT0 = __rdtsc(); // PROBE-KF
    if (useCopier) // PERF-GF
        st.cvSpace.wait(lock, [&] { return (st.q.empty() && st.copierQ.empty()) || st.queuedBytes + data.size() <= kVif1AsyncMaxBytes; });
    else
        st.cvSpace.wait(lock, [&] { return st.q.empty() || st.queuedBytes + data.size() <= kVif1AsyncMaxBytes; });
    g_waitTicks[1].fetch_add(__rdtsc() - eqT0, std::memory_order_relaxed);
    Vif1Job job;
    job.fbrst = fbrst;
    job.data = std::move(data);
    if (useCopier) // PERF-GF
        vif1PushCopierJobLocked(st, std::move(job));
    else
        vif1PushJobLocked(st, std::move(job));
}
static void vif1EnqueueSourceChainJob(uint32_t tadr, uint32_t chcr, uint32_t asr0, uint32_t asr1, uint32_t madr, uint32_t qwc) // PERF-GC
{
    Vif1AsyncState &st = vifAsync();
    const uint32_t fbrst = g_vif1FbrstProvider ? g_vif1FbrstProvider() : 0u;
    const bool useCopier = dma1CopierActive(); // PERF-GF
    std::unique_lock<std::mutex> lock(st.m);
    const uint64_t eqT0 = __rdtsc(); // PROBE-KF
    if (useCopier) // PERF-GF
        st.cvSpace.wait(lock, [&] { return (st.q.empty() && st.copierQ.empty()) || st.queuedBytes <= kVif1AsyncMaxBytes; });
    else
        st.cvSpace.wait(lock, [&] { return st.q.empty() || st.queuedBytes <= kVif1AsyncMaxBytes; });
    g_waitTicks[1].fetch_add(__rdtsc() - eqT0, std::memory_order_relaxed);
    Vif1Job job;
    job.fbrst = fbrst;
    job.isSourceChain = true;
    job.initialTadr = tadr;
    job.initialChcr = chcr;
    job.initialAsr0 = asr0;
    job.initialAsr1 = asr1;
    job.initialMadr = madr;
    job.initialQwc = qwc;
    job.copySeq = st.copyEnqueuedSeq.fetch_add(1, std::memory_order_acq_rel) + 1u;
    if (useCopier) // PERF-GF
        vif1PushCopierJobLocked(st, std::move(job));
    else
        vif1PushJobLocked(st, std::move(job));
}
static void vif1EnqueueCopy(const uint8_t *p, uint32_t n)
{
    std::vector<uint8_t> v = takeVifBuf(); // PERF-VP
    v.assign(p, p + n);
    vif1EnqueueJob(std::move(v));
}
extern "C" bool ps2_vif1_on_worker() { return t_onVif1Worker; }
extern "C" void ps2_vif1_mark_worker_thread() { t_onVif1Worker = true; }
// PERF-SL: called by the per-frame sceGsSyncPath: remember how many jobs exist now and wait until the worker has finished the jobs that existed `lag` calls ago.
// lag=1: at most one frame of VU1/GIF work is in flight while the EE builds the next one (steady cadence, bounded input latency).
extern "C" void ps2_vif1_sync_lag(uint32_t lag)
{
    if (!vif1AsyncEnabled() || t_onVif1Worker)
        return;
    Vif1AsyncState &st = vifAsync();
    static std::deque<uint64_t> s_tickets; // EE thread only
    std::unique_lock<std::mutex> lock(st.m);
    s_tickets.push_back(st.enqueuedJobs);
    if (s_tickets.size() > lag)
    {
        const uint64_t want = s_tickets.front();
        s_tickets.pop_front();
        if (st.doneJobs < want)
        {
            const uint64_t fT0 = __rdtsc();
            ++st.lagWaiters;
            if (g_ps2Ft2On) ps2_ft2_ev(6, want | (1ull << 62)); // PROBE-FT2: FENCE_IN (lag)
            st.cvIdle.wait(lock, [&] { return st.doneJobs >= want; });
            if (g_ps2Ft2On) ps2_ft2_ev(7, want | (1ull << 62)); // PROBE-FT2: FENCE_OUT (lag)
            --st.lagWaiters;
            g_waitTicks[0].fetch_add(__rdtsc() - fT0, std::memory_order_relaxed);
            g_waitTicks[5].fetch_add(1, std::memory_order_relaxed);
            if (g_tlOn) tlFence(fT0, __rdtsc()); // PROBE-TL
        }
    }
}
extern "C" void ps2_gs_async_stats(uint64_t *out); // PROBE-GQ
extern "C" void ps2_gs_front_stats(uint64_t *out); // PROBE-GQ2
extern "C" void ps2_gs_vk_stats(uint64_t *out);
extern "C" uint32_t probe_getNewestDistinctFn(); // PROBE-FS
extern thread_local uint32_t g_currentExecutingFunction;
struct FenceSite { uint32_t fn, cur; uintptr_t ra; uint64_t ticks, n; };
static FenceSite s_fenceSites[24]; // PROBE-FS: EE thread only (the worker returns before the wait)
std::atomic<uint32_t> g_vif1PublishedMark{0u};     // PERF-V1K
std::atomic<uint32_t> g_vif1PublishedStatBits{0u}; // PERF-V1K
std::atomic<bool> g_vif1PendingStc{false};         // PERF-V1K

extern "C" void ps2_vif1_fence()
{
    if (!vif1AsyncEnabled() || t_onVif1Worker)
        return;
    Vif1AsyncState &st = vifAsync();
    if (st.pending.load(std::memory_order_acquire))
    {
        const uint64_t fT0 = __rdtsc(); // PROBE-KF
        std::unique_lock<std::mutex> lock(st.m);
        if (g_ps2Ft2On) ps2_ft2_ev(6, reinterpret_cast<uintptr_t>(_ReturnAddress())); // PROBE-FT2: FENCE_IN
        st.cvIdle.wait(lock, [&] { return st.q.empty() && !st.busy && st.copierQ.empty() && !st.copierBusy; }); // PERF-GF
        if (g_ps2Ft2On) ps2_ft2_ev(7, reinterpret_cast<uintptr_t>(_ReturnAddress())); // PROBE-FT2: FENCE_OUT
        g_waitTicks[0].fetch_add(__rdtsc() - fT0, std::memory_order_relaxed);
        g_waitTicks[5].fetch_add(1, std::memory_order_relaxed);
        if (g_tlOn) tlFence(fT0, __rdtsc()); // PROBE-TL
        { // PROBE-FS
            const uint32_t fn = probe_getNewestDistinctFn(), cur = g_currentExecutingFunction;
            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            FenceSite *slot = nullptr;
            for (FenceSite &s : s_fenceSites)
                if (s.n && s.fn == fn && s.cur == cur && s.ra == ra) { slot = &s; break; }
            if (!slot)
                for (FenceSite &s : s_fenceSites)
                    if (!s.n) { slot = &s; s.fn = fn; s.cur = cur; s.ra = ra; break; }
            if (slot) { slot->ticks += __rdtsc() - fT0; ++slot->n; }
        }
    }
    if (g_vif1ApplyVpuStat)
        g_vif1ApplyVpuStat();
    // PROBE-KF: once per second (EE thread): where does the EE wait? ms/s per wait source + VIF1 worker busy
    static uint64_t s_calls = 0;
    if ((++s_calls & 0xFFFu) == 0u)
    {
        static auto s_t0 = std::chrono::steady_clock::now();
        static uint64_t s_tsc0 = __rdtsc();
        static uint64_t s_prev[8]{};
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - s_t0).count();
        if (ms >= 1000.0)
        {
            const uint64_t tsc = __rdtsc();
            const double tpm = static_cast<double>(tsc - s_tsc0) / ms;
            double v[8];
            for (int i = 0; i < 7; ++i)
            {
                const uint64_t cur = g_waitTicks[i].load(std::memory_order_relaxed);
                v[i] = (i == 5) ? static_cast<double>(cur - s_prev[i]) : static_cast<double>(cur - s_prev[i]) / tpm;
                s_prev[i] = cur;
            }
            // global ps2xSafeLog
            ps2xSafeLog("[wait] window_ms=%.0f EE ms: vif1_fence=%.0f (n=%.0f) vif1_enqueue=%.0f gs_fence=%.0f gs_backpressure=%.0f | vif1_worker_busy=%.0f vif1_worker_idle=%.0f\n", ms, v[0], v[5],
                        v[1], v[2], v[3], v[4], v[6]);
            if (g_tlOn) // PROBE-TL
            {
                const double nj = static_cast<double>(g_tlJobs.exchange(0, std::memory_order_relaxed));
                const double nf = static_cast<double>(g_tlFences.exchange(0, std::memory_order_relaxed));
                const double q = static_cast<double>(g_tlQueue.exchange(0, std::memory_order_relaxed));
                const double d = static_cast<double>(g_tlDur.exchange(0, std::memory_order_relaxed));
                const double dm = static_cast<double>(g_tlDurMax.exchange(0, std::memory_order_relaxed));
                const double fw = static_cast<double>(g_tlFWait.exchange(0, std::memory_order_relaxed));
                const double ld = static_cast<double>(g_tlLead.exchange(0, std::memory_order_relaxed));
                ps2xSafeLog("[tl] jobs=%llu queue_ms=%.2f dur_ms=%.2f durmax_ms=%.2f | fences=%llu fwait_ms=%.2f lead_ms=%.2f\n", (unsigned long long)nj, nj > 0 ? q / nj / tpm : 0.0,
                            nj > 0 ? d / nj / tpm : 0.0, dm / tpm, (unsigned long long)nf, nf > 0 ? fw / nf / tpm : 0.0, nf > 0 ? ld / nf / tpm : 0.0);
            }
            { // PROBE-GQ
                uint64_t g[5];
                ps2_gs_async_stats(g);
                ps2xSafeLog("[gsq] gs_busy_ms=%.0f gs_idle_ms=%.0f producer_blocks=%llu peak_queue_kb=%.0f packets=%llu\n", static_cast<double>(g[0]) / tpm, static_cast<double>(g[1]) / tpm,
                            (unsigned long long)g[2], static_cast<double>(g[3]) / 1024.0, (unsigned long long)g[4]);
            }
            { // PROBE-GQ2
                uint64_t f[3], k[4];
                ps2_gs_front_stats(f);
                ps2_gs_vk_stats(k);
                ps2xSafeLog("[gsq2] draws=%llu build_ms=%.0f submit_ms=%.0f | vk_lock_wait_ms=%.0f (n=%llu) present_hold_ms=%.0f (n=%llu)\n", (unsigned long long)f[2], static_cast<double>(f[0]) / tpm,
                            static_cast<double>(f[1]) / tpm, static_cast<double>(k[0]) / tpm, (unsigned long long)k[1], static_cast<double>(k[2]) / tpm, (unsigned long long)k[3]);
            }
            { // PROBE-FS: the heaviest waiting call sites of this window
                for (int pass = 0; pass < 4; ++pass)
                {
                    FenceSite *best = nullptr;
                    for (FenceSite &s : s_fenceSites)
                        if (s.n && (!best || s.ticks > best->ticks)) best = &s;
                    if (!best) break;
                    ps2xSafeLog("[fsite] newestFn=%08X curFn=%08X ra=%p waits=%llu ms=%.1f\n", best->fn, best->cur, reinterpret_cast<void *>(best->ra),
                                (unsigned long long)best->n, static_cast<double>(best->ticks) / tpm);
                    best->n = 0; best->ticks = 0;
                }
                for (FenceSite &s : s_fenceSites) { s.n = 0; s.ticks = 0; }
            }
            s_t0 = now;
            s_tsc0 = tsc;
        }
    }
}
static std::mutex s_fkSpanMutex;
static std::map<uint32_t, std::vector<probe_ae::AySpan>> s_fkSpansBySize;
static void ps2_fk_setSpans(uint32_t chainSize, const std::vector<probe_ae::AySpan> &spans)
{
    std::lock_guard<std::mutex> lock(s_fkSpanMutex);
    s_fkSpansBySize[chainSize] = spans;
}
extern "C" bool ps2_fk_lookup(uint32_t chainSize, uint32_t pos, uint32_t *outSrc, uint32_t *outBytes, uint32_t *outBufOff)
{
    std::lock_guard<std::mutex> lock(s_fkSpanMutex);
    auto it = s_fkSpansBySize.find(chainSize);
    if (it == s_fkSpansBySize.end())
        return false;
    for (const auto &sp : it->second)
    {
        if (sp.byteCount != 8u && pos >= sp.bufOffset && pos < sp.bufOffset + sp.byteCount)
        {
            *outSrc = sp.srcGuestAddr + (pos - sp.bufOffset);
            *outBytes = sp.byteCount;
            *outBufOff = sp.bufOffset;
            return true;
        }
    }
    return false;
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    constexpr uint32_t kGifStat = 0x10003020u;
    constexpr uint32_t kGifFqcMask = 0x1F000000u;
    if (s_gifStatOwner != this || s_gifStatPtr == nullptr) // PERF-KL: unordered_map elements are node-stable; the only invalidation is clear() (reset above)
    {
        auto gifStatIt = m_ioRegisters.find(kGifStat);
        s_gifStatOwner = this;
        s_gifStatPtr = gifStatIt != m_ioRegisters.end() ? &gifStatIt->second : nullptr;
    }
    if (s_gifStatPtr)
        *s_gifStatPtr &= ~kGifFqcMask;

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    if (physAddr >= 0x11000000u && physAddr < 0x11010000u)
        ps2_vif1_fence(); // PERF-HN
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onGsPrivRegister(address, newVal);
            }
#endif
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
#if defined(PS2X_PROBE_AE) || 1
            if (probe_ae::isEnabled()) {
                probe_ae::onGsPrivRegister(address, value);
            }
#endif
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        if (vif1AsyncEnabled())
        {
            if (!s_vif1Process)
            {
                s_vif1DrainArbiter = [this]() { if (m_gifArbiter) m_gifArbiter->drain(); };
                s_vif1Process = [this](const uint8_t *d, uint32_t n) { processVIF1Data(d, n); };
            }
            vif1EnqueueCopy(fifoData, sizeof(fifoData));
            return;
        }
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (!t_onVif1Worker && ((address & 0xFFFFFF00u) == 0x10009000u || address == 0x1000E010u)) // PERF-GC
    {
        waitDma1CopyDone();
        syncDma1PublishedToIoRegs(m_ioRegisters);
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        if (address == 0x10003C10u && value == 0x8u && !t_onVif1Worker && markNoFenceEnabled()) // PERF-V1K: FBRST.STC only
        {
            m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
            g_vif1PublishedStatBits.fetch_and(~kVif1StcClearMask, std::memory_order_acq_rel);
            g_vif1PendingStc.store(true, std::memory_order_release);
            return true;
        }

        ps2_vif1_fence(); // PERF-HN
        if (g_vif1PendingStc.exchange(false, std::memory_order_acq_rel)) // PERF-V1K
            vif1_regs.stat &= ~kVif1StcClearMask;
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                g_vif1PublishedMark.store(0u, std::memory_order_release);     // PERF-V1K
                g_vif1PublishedStatBits.store(0u, std::memory_order_release); // PERF-V1K
                g_vif1PendingStc.store(false, std::memory_order_release);     // PERF-V1K
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~kVif1StcClearMask;
                g_vif1PublishedStatBits.fetch_and(~kVif1StcClearMask, std::memory_order_acq_rel); // PERF-V1K
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            m_ioRegisters[0x10003C30u] = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            g_vif1PublishedMark.store(value & 0xFFFFu, std::memory_order_release);      // PERF-V1K
            g_vif1PublishedStatBits.fetch_and(~(1u << 6), std::memory_order_acq_rel);   // PERF-V1K
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            {
                // Task FU: first sighting of each (channel, MOD, direction, TTE) DMA combination + periodic counts
                static std::mutex s_fuMutex;
                static std::map<uint32_t, uint32_t> s_fuSeen;
                const uint32_t fuKey = (channelBase & 0xFFFF00u) | (((value >> 2u) & 3u) << 4u) | (value & 1u) | (((value >> 6u) & 1u) << 1u);
                std::lock_guard<std::mutex> fuLock(s_fuMutex);
                uint32_t &fuCount = s_fuSeen[fuKey];
                if (fuCount == 0u || (fuCount % 20000u) == 0u)
                {
                    std::printf("[probe] fu dma ch=0x%08X chcr=0x%08X mod=%u dir=%u tte=%u madr=0x%08X tadr=0x%08X qwc=%u count=%u\n",
                                channelBase, value, (value >> 2u) & 3u, value & 1u, (value >> 6u) & 1u,
                                m_ioRegisters[channelBase + 0x10u], m_ioRegisters[channelBase + 0x30u],
                                m_ioRegisters[channelBase + 0x20u] & 0xFFFFu, fuCount);
                    std::fflush(stdout);
                }
                ++fuCount;
            }

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                    {
                        m_pendingVif1Transfers.push_back(pt);
                        g_gsFrameKicks.fetch_add(1, std::memory_order_release);
                    }
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    if (!s_vif1Process) // PERF-GC
                    {
                        s_vif1DrainArbiter = [this]() { if (m_gifArbiter) m_gifArbiter->drain(); };
                        s_vif1Process = [this](const uint8_t *d, uint32_t n) { processVIF1Data(d, n); };
                    }
                    if (!s_vif1QueueDmacCause) // PERF-GC
                    {
                        s_vif1QueueDmacCause = [this](uint32_t cause) { queueCompletedDmacCause(cause); };
                    }
                    if (!s_vif1WalkChain) // PERF-GC
                    {
                        s_vif1WalkChain = [this](uint32_t channelBase, uint32_t chcrIn, uint32_t tagAddrIn, uint32_t asr0In, uint32_t asr1In, std::vector<uint8_t> &chainBuf) -> DmaChainWalkResult
                        {
                            auto walkOld = [this, channelBase](bool suppressProbes, uint32_t chcr, uint32_t tagAddr, uint32_t asr0, uint32_t asr1, std::vector<uint8_t> &chainBuf) -> DmaChainWalkResult
                            {
                                const uint32_t initialTagAddr = tagAddr;
                                const bool scanOnly = t_vif1ScanOnly; // PERF-GC
                                if (!suppressProbes && channelBase == 0x10009000u && !scanOnly) {
#if defined(PS2X_PROBE_AE) || 1
                                    if (probe_ae::isEnabled()) {
                                        probe_ae::onAwDma1KickStart(tagAddr, chcr);
                                    }
#endif
                                }
                                uint32_t asp = (chcr >> 4) & 0x3u;
                                const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                                const int kMaxChainTags = 1 << 20;
                                std::vector<probe_ae::AySpan> aySpans;
                                auto tChainStart = std::chrono::steady_clock::now();
                                uint32_t spanCount = 0u;

                                auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                                {
                                    if (scanOnly) // PERF-GC
                                    {
                                        if (isScratchpad(srcAddr))
                                            t_vif1ScanHitSpr = true;
                                        return;
                                    }
                                    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                                    uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                                    if (!suppressProbes && channelBase == 0x10009000u) {
                                        aySpans.push_back({ (uint32_t)chainBuf.size(), bytes, srcAddr });
                                    }
                                    const bool scratch = isScratchpad(srcAddr);
                                    uint32_t src = 0;
                                    src = translateAddress(srcAddr);
                                    const uint8_t *base2;
                                    uint32_t maxSz2;
                                    if (scratch)
                                    {
                                        base2 = m_scratchpad;
                                        maxSz2 = PS2_SCRATCHPAD_SIZE;
                                    }
                                    else
                                    {
                                        base2 = m_rdram;
                                        maxSz2 = PS2_RAM_SIZE;
                                    }

                                    while (bytes > 0)
                                    {
                                        if (src >= maxSz2)
                                            src = 0;
                                        uint32_t chunk = bytes;
                                        if (src + chunk > maxSz2)
                                            chunk = maxSz2 - src;
                                        if (chunk == 0)
                                            break;
                                        chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                                        ++spanCount;
                                        bytes -= chunk;
                                        src += chunk;
                                    }
                                };

                                auto appendVifTagData = [&](uint32_t localTagAddr)
                                {
                                    uint32_t tagPhys = 0u;
                                    const bool tagScratch = isScratchpad(localTagAddr);
                                    tagPhys = translateAddress(localTagAddr);

                                    const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                                    const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                    if (tagPhys + 16u > localMax)
                                        return;

                                    if (!suppressProbes && channelBase == 0x10009000u) {
                                        aySpans.push_back({ (uint32_t)chainBuf.size(), 8u, localTagAddr + 8u });
                                    }

                                    // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                                    // the tag payload. VIF chains use those bytes for two VIFcodes.
                                    chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                                    ++spanCount;
                                };

                                const bool isVifChannel =
                                    channelBase == 0x10009000u || channelBase == 0x10008000u;
                                const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                                int tagsProcessed = 0;
                                uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                                uint32_t lastTagId = 0;
                                const uint32_t aw3ChcrStart = chcr; // PROBE-AW3
                                const bool aw3Track = !suppressProbes && !scanOnly && (channelBase == 0x10009000u) && aw3ProbeOn(); // PROBE-AW3
                                if (aw3Track) g_aw3SawMark2000.store(false, std::memory_order_relaxed); // PROBE-AW3
                                const char *aw3EndReason = "other"; // PROBE-AW3

                                while (tagsProcessed < kMaxChainTags)
                                {
                                    const uint32_t currentTagAddr = tagAddr;
                                    const bool tagInSPR = isScratchpad(tagAddr);
                                    if (scanOnly && tagInSPR) // PERF-GC
                                    {
                                        t_vif1ScanHitSpr = true;
                                        aw3EndReason = "spr"; // PROBE-AW3
                                        break;
                                    }
                                    uint32_t physTag = 0;
                                    try
                                    {
                                        physTag = translateAddress(tagAddr);
                                    }
                                    catch (...)
                                    {
                                        aw3EndReason = "bad-addr"; // PROBE-AW3
                                        break;
                                    }
                                    const uint8_t *tagBase;
                                    uint32_t tagMax;
                                    if (tagInSPR)
                                    {
                                        tagBase = m_scratchpad;
                                        tagMax = PS2_SCRATCHPAD_SIZE;
                                    }
                                    else
                                    {
                                        tagBase = m_rdram;
                                        tagMax = PS2_RAM_SIZE;
                                    }
                                    if (physTag + 16 > tagMax)
                                    { // PROBE-AW3
                                        aw3EndReason = tagInSPR ? "spr" : "oob"; // PROBE-AW3
                                        break;
                                    } // PROBE-AW3

                                    const uint8_t *tp = tagBase + physTag;
                                    uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                                    uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                                    uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                                    lastTagId = id;
                                    const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                                    uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                                    lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                                    ++tagsProcessed;

                                    uint32_t dataAddr = 0;
                                    bool hasPayload = (tagQwc > 0);
                                    bool endChain = false;

                                    switch (id)
                                    {
                                    case 0:
                                        dataAddr = addr;
                                        tagAddr = tagAddr + 16;
                                        endChain = true;
                                        aw3EndReason = "refe0"; // PROBE-AW3
                                        break;
                                    case 1:
                                        dataAddr = tagAddr + 16;
                                        tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                        break;
                                    case 2:
                                        dataAddr = tagAddr + 16;
                                        tagAddr = addr;
                                        break;
                                    case 3:
                                    case 4:
                                        dataAddr = addr;
                                        tagAddr = tagAddr + 16;
                                        break;
                                    case 5:
                                        dataAddr = tagAddr + 16;
                                        {
                                            const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                            if (asp == 0u)
                                            {
                                                asr0 = retAddr;
                                                asp = 1u;
                                            }
                                            else if (asp == 1u)
                                            {
                                                asr1 = retAddr;
                                                asp = 2u;
                                            }
                                        }
                                        tagAddr = addr;
                                        break;
                                    case 6:
                                        dataAddr = tagAddr + 16;
                                        if (asp == 2u)
                                        {
                                            tagAddr = asr1;
                                            asp = 1u;
                                        }
                                        else if (asp == 1u)
                                        {
                                            tagAddr = asr0;
                                            asp = 0u;
                                        }
                                        else
                                        {
                                            endChain = true;
                                            aw3EndReason = "ret-empty6"; // PROBE-AW3
                                        }
                                        break;
                                    case 7:
                                        dataAddr = tagAddr + 16;
                                        endChain = true;
                                        aw3EndReason = "end7"; // PROBE-AW3
                                        break;
                                    default:
                                        hasPayload = false;
                                        endChain = true;
                                        aw3EndReason = "bad-id"; // PROBE-AW3
                                        break;
                                    }

                                    if (scanOnly) // PERF-GC: tag-only scan, no copy / probes
                                    {
                                        if (hasPayload)
                                            appendData(dataAddr, tagQwc);
                                        if (t_vif1ScanHitSpr || endChain || (irq && tieEnabled))
                                            break;
                                        continue;
                                    }

                                    if (transferTagData)
                                        appendVifTagData(currentTagAddr);

                                    if (hasPayload)
                                        appendData(dataAddr, tagQwc);

                                    if (!suppressProbes && channelBase == 0x10009000u)
                                    {
#if defined(PS2X_PROBE_AE) || 1
                                        if (probe_ae::isEnabled()) {
                                            uint32_t sampleWords[8] = {0};
                                            uint32_t numWords = 0;
                                            if (hasPayload && tagQwc > 0) {
                                                uint32_t dp = 0;
                                                try { dp = translateAddress(dataAddr); } catch(...) {}
                                                const uint8_t *db = isScratchpad(dataAddr) ? m_scratchpad : m_rdram;
                                                const uint32_t dm = isScratchpad(dataAddr) ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                                uint32_t maxBytes = std::min<uint32_t>(32u, static_cast<uint32_t>(tagQwc) * 16u);
                                                if (dp + maxBytes <= dm) {
                                                    std::memcpy(sampleWords, db + dp, maxBytes);
                                                    numWords = maxBytes / 4;
                                                }
                                            }
                                            probe_ae::onAwDma1Tag(currentTagAddr, id, tagQwc, tagAddr, dataAddr, sampleWords, numWords);
                                        }
#endif
                                    }

                                    if (!suppressProbes && channelBase == 0x10009000u && (tagQwc == 512 || tagQwc == 64 || (tagQwc > 0 && tagsProcessed <= 6)))
                                    {
                                        uint32_t w2 = 0, w3 = 0;
                                        if (physTag + 16 <= tagMax) {
                                            std::memcpy(&w2, tp + 8, 4);
                                            std::memcpy(&w3, tp + 12, 4);
                                        }
                                        uint8_t sample[16] = {};
                                        if (hasPayload) {
                                            uint32_t dp = 0;
                                            try { dp = translateAddress(dataAddr); } catch(...) {}
                                            const uint8_t *db = isScratchpad(dataAddr) ? m_scratchpad : m_rdram;
                                            const uint32_t dm = isScratchpad(dataAddr) ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                            if (dp + 16 <= dm) std::memcpy(sample, db + dp, 16);
                                        }
                                        static int s_tagLogCount = 0;
                                        static int s_vifTagVerbose = -1;
                                        if (s_vifTagVerbose == -1) {
                                            const char *v = std::getenv("PS2X_VERBOSE_LOG");
                                            s_vifTagVerbose = (v && (v[0] == '1' || v[0] == 't')) ? 1 : 0;
                                        }
                                        if (s_vifTagVerbose && s_tagLogCount++ < 50) {
                                            std::fprintf(stderr, "[DMA:VIF1_TAG] tagAddr=0x%08X id=%u qwc=%u dataAddr=0x%08X w2=0x%08X w3=0x%08X TTE=%d payload0..7=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
                                                         currentTagAddr, id, tagQwc, dataAddr, w2, w3, transferTagData ? 1 : 0,
                                                         sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6], sample[7]);
                                            std::fflush(stderr);
                                        }
                                    }
                                    if (irq && tieEnabled)
                                    { // PROBE-AW3
                                        if (!endChain) aw3EndReason = "irq+tie"; // PROBE-AW3
                                        endChain = true;
                                    } // PROBE-AW3
                                    if (endChain)
                                        break;
                                }

                                if (scanOnly) // PERF-GC
                                    return DmaChainWalkResult{tagAddr, asr0, asr1, chcr, static_cast<uint32_t>(tagsProcessed), 0u};

                                if (tagsProcessed >= kMaxChainTags && std::strcmp(aw3EndReason, "other") == 0) // PROBE-AW3
                                    aw3EndReason = "cap"; // PROBE-AW3

                                if (!suppressProbes && tagsProcessed >= kMaxChainTags)
                                {
                                    static bool s_capHitReported = false;
                                    if (!s_capHitReported)
                                    {
                                        s_capHitReported = true;
                                        std::fprintf(stderr, "[DMA] chain truncated at kMaxChainTags ch=0x%X tag=0x%08X\n", channelBase, tagAddr);
                                        std::fflush(stderr);
                                        std::printf("[probe] ax cap_hit ch=0x%X tag=0x%08X\n", channelBase, tagAddr);
                                        std::fflush(stdout);
                                    }
                                }

                                if (!suppressProbes)
                                {
#if defined(PS2X_PROBE_AE) || 1
                                    if ((probe_ae::isEnabled() || probe_ae::isMovieHleActive() || probe_ae::isMovieHleVerify()) && channelBase == 0x10009000u)
                                    {
                                        if (probe_ae::isEnabled())
                                        {
                                            static int s_over2kCount = 0;
                                            if ((tagsProcessed > 2000 && s_over2kCount++ < 5) || (tagsProcessed > 4096))
                                            {
                                                std::printf("[probe] ax chain tags=%d bytes=%zu end=%u\n", tagsProcessed, chainBuf.size(), lastTagId);
                                                std::fflush(stdout);
                                            }
                                            auto tChainEnd = std::chrono::steady_clock::now();
                                            probe_ae::onAyTimeChain(std::chrono::duration_cast<std::chrono::microseconds>(tChainEnd - tChainStart).count());
                                            probe_ae::onAyKick();
                                            probe_ae::setRdram(m_rdram);
                                            probe_ae::onAyChainStart(tagsProcessed, aySpans);
                                        }
                                        probe_ae::setRdram(m_rdram);
                                        probe_ae::onBbChainStart(initialTagAddr, tagsProcessed, aySpans);
                                    }
#endif

                                    if (channelBase == 0x10009000u && !chainBuf.empty())
                                    {
                                        static const bool s_fkOn = []() { const char *v = std::getenv("PS2X_PROBE_VU3D"); return v && v[0] == '1'; }();
                                        if (s_fkOn)
                                            ps2_fk_setSpans(static_cast<uint32_t>(chainBuf.size()), aySpans);
                                    }
                                }
                                chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                                chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                                if (aw3Track) // PROBE-AW3
                                    aw3RecordChain('A', initialTagAddr, static_cast<uint32_t>(tagsProcessed), aw3EndReason, tagAddr, aw3ChcrStart); // PROBE-AW3
                                return DmaChainWalkResult{tagAddr, asr0, asr1, chcr, static_cast<uint32_t>(tagsProcessed), spanCount};
                            };

                            auto walkNew = [this, channelBase](uint32_t chcr, uint32_t tagAddr, uint32_t asr0, uint32_t asr1, std::vector<uint8_t> &chainBuf) -> DmaChainWalkResult // PERF-GG
                            {
                                const uint32_t initialTagAddr = tagAddr;
                                const bool scanOnly = t_vif1ScanOnly;
                                const bool probeEnabled = !scanOnly && probe_ae::isEnabled();
                                static const bool s_fkOn = []() { const char *v = std::getenv("PS2X_PROBE_VU3D"); return v && v[0] == '1'; }();
                                const bool isVif1 = (channelBase == 0x10009000u);
                                const bool needSpans = !scanOnly && isVif1 && (probeEnabled || probe_ae::isMovieHleActive() || probe_ae::isMovieHleVerify() || s_fkOn);

                                if (isVif1 && probeEnabled)
                                    probe_ae::onAwDma1KickStart(tagAddr, chcr);

                                uint32_t asp = (chcr >> 4) & 0x3u;
                                const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                                constexpr int kMaxChainTags = 1 << 20;
                                std::vector<probe_ae::AySpan> aySpans;
                                std::chrono::steady_clock::time_point tChainStart;
                                if (isVif1 && probeEnabled)
                                    tChainStart = std::chrono::steady_clock::now();

                                static std::atomic<size_t> s_lastChainSize{65536u};
                                if (!scanOnly)
                                {
                                    const size_t hint = s_lastChainSize.load(std::memory_order_relaxed);
                                    if (chainBuf.capacity() < hint)
                                        chainBuf.reserve(hint);
                                }

                                const uint8_t *pendingSrc = nullptr;
                                uint32_t pendingBytes = 0u;
                                uint32_t spanCount = 0u;

                                auto flushPending = [&]()
                                {
                                    if (pendingBytes == 0u)
                                        return;
                                    const size_t cur = chainBuf.size();
                                    const size_t need = cur + pendingBytes;
                                    if (need > chainBuf.capacity())
                                    {
                                        size_t newCap = chainBuf.capacity() ? chainBuf.capacity() * 2u : 4096u;
                                        if (newCap < need)
                                            newCap = need;
                                        chainBuf.reserve(newCap);
                                    }
                                    chainBuf.insert(chainBuf.end(), pendingSrc, pendingSrc + pendingBytes);
                                    pendingSrc = nullptr;
                                    pendingBytes = 0u;
                                    ++spanCount;
                                };

                                auto emitSpan = [&](const uint8_t *srcPtr, uint32_t chunk)
                                {
                                    if (pendingBytes > 0u && pendingSrc + pendingBytes == srcPtr && pendingBytes <= 0xFFFFFFFFu - chunk)
                                    {
                                        pendingBytes += chunk;
                                        return;
                                    }
                                    flushPending();
                                    pendingSrc = srcPtr;
                                    pendingBytes = chunk;
                                };

                                const bool isVifChannel = isVif1 || (channelBase == 0x10008000u);
                                const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                                int tagsProcessed = 0;
                                uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                                uint32_t lastTagId = 0u;
                                const uint32_t aw3ChcrStart = chcr; // PROBE-AW3
                                const bool aw3Track = !scanOnly && isVif1 && aw3ProbeOn(); // PROBE-AW3
                                if (aw3Track) g_aw3SawMark2000.store(false, std::memory_order_relaxed); // PROBE-AW3
                                const char *aw3EndReason = "other"; // PROBE-AW3

                                while (tagsProcessed < kMaxChainTags)
                                {
                                    const uint32_t currentTagAddr = tagAddr;
                                    const uint8_t *tp = nullptr;
                                    uint32_t physTag = 0u;
                                    uint32_t tagMax = PS2_RAM_SIZE;

                                    if (tagAddr <= PS2_RAM_SIZE - 16u)
                                    {
                                        physTag = tagAddr;
                                        tp = m_rdram + physTag;
                                    }
                                    else
                                    {
                                        const bool tagInSPR = isScratchpad(tagAddr);
                                        if (scanOnly && tagInSPR)
                                        {
                                            t_vif1ScanHitSpr = true;
                                            aw3EndReason = "spr"; // PROBE-AW3
                                            break;
                                        }
                                        try
                                        {
                                            physTag = translateAddress(tagAddr);
                                        }
                                        catch (...)
                                        {
                                            aw3EndReason = "bad-addr"; // PROBE-AW3
                                            break;
                                        }
                                        const uint8_t *tagBase = tagInSPR ? m_scratchpad : m_rdram;
                                        tagMax = tagInSPR ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                        if (physTag + 16u > tagMax)
                                        { // PROBE-AW3
                                            aw3EndReason = tagInSPR ? "spr" : "oob"; // PROBE-AW3
                                            break;
                                        } // PROBE-AW3
                                        tp = tagBase + physTag;
                                    }

                                    uint64_t tag = 0ull;
                                    std::memcpy(&tag, tp, sizeof(tag));
                                    const uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFFu);
                                    const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
                                    lastTagId = id;
                                    const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                                    const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFFull);
                                    lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                                    ++tagsProcessed;

                                    uint32_t dataAddr = 0u;
                                    bool hasPayload = (tagQwc > 0u);
                                    bool endChain = false;

                                    switch (id)
                                    {
                                    case 0:
                                        dataAddr = addr;
                                        tagAddr = tagAddr + 16u;
                                        endChain = true;
                                        aw3EndReason = "refe0"; // PROBE-AW3
                                        break;
                                    case 1:
                                        dataAddr = tagAddr + 16u;
                                        tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                        break;
                                    case 2:
                                        dataAddr = tagAddr + 16u;
                                        tagAddr = addr;
                                        break;
                                    case 3:
                                    case 4:
                                        dataAddr = addr;
                                        tagAddr = tagAddr + 16u;
                                        break;
                                    case 5:
                                        dataAddr = tagAddr + 16u;
                                        {
                                            const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                            if (asp == 0u)
                                            {
                                                asr0 = retAddr;
                                                asp = 1u;
                                            }
                                            else if (asp == 1u)
                                            {
                                                asr1 = retAddr;
                                                asp = 2u;
                                            }
                                        }
                                        tagAddr = addr;
                                        break;
                                    case 6:
                                        dataAddr = tagAddr + 16u;
                                        if (asp == 2u)
                                        {
                                            tagAddr = asr1;
                                            asp = 1u;
                                        }
                                        else if (asp == 1u)
                                        {
                                            tagAddr = asr0;
                                            asp = 0u;
                                        }
                                        else
                                        {
                                            endChain = true;
                                            aw3EndReason = "ret-empty6"; // PROBE-AW3
                                        }
                                        break;
                                    case 7:
                                        dataAddr = tagAddr + 16u;
                                        endChain = true;
                                        aw3EndReason = "end7"; // PROBE-AW3
                                        break;
                                    default:
                                        hasPayload = false;
                                        endChain = true;
                                        aw3EndReason = "bad-id"; // PROBE-AW3
                                        break;
                                    }

                                    if (scanOnly)
                                    {
                                        if (hasPayload && dataAddr >= PS2_RAM_SIZE && isScratchpad(dataAddr))
                                            t_vif1ScanHitSpr = true;
                                        if (t_vif1ScanHitSpr || endChain || (irq && tieEnabled))
                                            break;
                                        continue;
                                    }

                                    if (transferTagData)
                                    {
                                        if (needSpans)
                                            aySpans.push_back({ static_cast<uint32_t>(chainBuf.size() + pendingBytes), 8u, currentTagAddr + 8u });
                                        emitSpan(tp + 8u, 8u);
                                    }

                                    if (hasPayload)
                                    {
                                        const uint64_t bytes64 = static_cast<uint64_t>(tagQwc) * 16ull;
                                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                                        if (needSpans)
                                            aySpans.push_back({ static_cast<uint32_t>(chainBuf.size() + pendingBytes), bytes, dataAddr });

                                        const uint8_t *base2;
                                        uint32_t maxSz2;
                                        uint32_t src;
                                        if (dataAddr < PS2_RAM_SIZE)
                                        {
                                            base2 = m_rdram;
                                            maxSz2 = PS2_RAM_SIZE;
                                            src = dataAddr;
                                        }
                                        else if ((dataAddr & 0xE0000000u) == 0x20000000u)
                                        {
                                            base2 = m_rdram;
                                            maxSz2 = PS2_RAM_SIZE;
                                            src = dataAddr & PS2_RAM_MASK;
                                        }
                                        else
                                        {
                                            const bool scratch = isScratchpad(dataAddr);
                                            src = translateAddress(dataAddr);
                                            base2 = scratch ? m_scratchpad : m_rdram;
                                            maxSz2 = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                        }

                                        if (src < maxSz2 && bytes <= maxSz2 - src)
                                        {
                                            emitSpan(base2 + src, bytes);
                                        }
                                        else
                                        {
                                            while (bytes > 0u)
                                            {
                                                if (src >= maxSz2)
                                                    src = 0u;
                                                uint32_t chunk = bytes;
                                                if (src + chunk > maxSz2)
                                                    chunk = maxSz2 - src;
                                                if (chunk == 0u)
                                                    break;
                                                emitSpan(base2 + src, chunk);
                                                bytes -= chunk;
                                                src += chunk;
                                            }
                                        }
                                    }

                                    if (probeEnabled && isVif1)
                                    {
                                        uint32_t sampleWords[8] = {0};
                                        uint32_t numWords = 0;
                                        if (hasPayload && tagQwc > 0) {
                                            uint32_t dp = 0;
                                            try { dp = translateAddress(dataAddr); } catch(...) {}
                                            const uint8_t *db = isScratchpad(dataAddr) ? m_scratchpad : m_rdram;
                                            const uint32_t dm = isScratchpad(dataAddr) ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                            uint32_t maxBytes = std::min<uint32_t>(32u, static_cast<uint32_t>(tagQwc) * 16u);
                                            if (dp + maxBytes <= dm) {
                                                std::memcpy(sampleWords, db + dp, maxBytes);
                                                numWords = maxBytes / 4;
                                            }
                                        }
                                        probe_ae::onAwDma1Tag(currentTagAddr, id, tagQwc, tagAddr, dataAddr, sampleWords, numWords);
                                    }

                                    static int s_vifTagVerbose = -1;
                                    if (s_vifTagVerbose == -1) {
                                        const char *v = std::getenv("PS2X_VERBOSE_LOG");
                                        s_vifTagVerbose = (v && (v[0] == '1' || v[0] == 't')) ? 1 : 0;
                                    }
                                    if (s_vifTagVerbose && isVif1 && (tagQwc == 512 || tagQwc == 64 || (tagQwc > 0 && tagsProcessed <= 6)))
                                    {
                                        uint32_t w2 = 0, w3 = 0;
                                        if (physTag + 16 <= tagMax) {
                                            std::memcpy(&w2, tp + 8, 4);
                                            std::memcpy(&w3, tp + 12, 4);
                                        }
                                        uint8_t sample[16] = {};
                                        if (hasPayload) {
                                            uint32_t dp = 0;
                                            try { dp = translateAddress(dataAddr); } catch(...) {}
                                            const uint8_t *db = isScratchpad(dataAddr) ? m_scratchpad : m_rdram;
                                            const uint32_t dm = isScratchpad(dataAddr) ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                                            if (dp + 16 <= dm) std::memcpy(sample, db + dp, 16);
                                        }
                                        static int s_tagLogCount = 0;
                                        if (s_tagLogCount++ < 50) {
                                            std::fprintf(stderr, "[DMA:VIF1_TAG] tagAddr=0x%08X id=%u qwc=%u dataAddr=0x%08X w2=0x%08X w3=0x%08X TTE=%d payload0..7=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
                                                         currentTagAddr, id, tagQwc, dataAddr, w2, w3, transferTagData ? 1 : 0,
                                                         sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6], sample[7]);
                                            std::fflush(stderr);
                                        }
                                    }
                                    if (irq && tieEnabled)
                                    { // PROBE-AW3
                                        if (!endChain) aw3EndReason = "irq+tie"; // PROBE-AW3
                                        endChain = true;
                                    } // PROBE-AW3
                                    if (endChain)
                                        break;
                                }

                                if (scanOnly)
                                    return DmaChainWalkResult{tagAddr, asr0, asr1, chcr, static_cast<uint32_t>(tagsProcessed), 0u};

                                if (tagsProcessed >= kMaxChainTags && std::strcmp(aw3EndReason, "other") == 0) // PROBE-AW3
                                    aw3EndReason = "cap"; // PROBE-AW3

                                flushPending();
                                if (!chainBuf.empty())
                                    s_lastChainSize.store(chainBuf.size(), std::memory_order_relaxed);

                                if (tagsProcessed >= kMaxChainTags)
                                {
                                    static bool s_capHitReported = false;
                                    if (!s_capHitReported)
                                    {
                                        s_capHitReported = true;
                                        std::fprintf(stderr, "[DMA] chain truncated at kMaxChainTags ch=0x%X tag=0x%08X\n", channelBase, tagAddr);
                                        std::fflush(stderr);
                                        std::printf("[probe] ax cap_hit ch=0x%X tag=0x%08X\n", channelBase, tagAddr);
                                        std::fflush(stdout);
                                    }
                                }

                                if (isVif1 && (probeEnabled || probe_ae::isMovieHleActive() || probe_ae::isMovieHleVerify()))
                                {
                                    if (probeEnabled)
                                    {
                                        static int s_over2kCount = 0;
                                        if ((tagsProcessed > 2000 && s_over2kCount++ < 5) || (tagsProcessed > 4096))
                                        {
                                            std::printf("[probe] ax chain tags=%d bytes=%zu end=%u\n", tagsProcessed, chainBuf.size(), lastTagId);
                                            std::fflush(stdout);
                                        }
                                        auto tChainEnd = std::chrono::steady_clock::now();
                                        probe_ae::onAyTimeChain(std::chrono::duration_cast<std::chrono::microseconds>(tChainEnd - tChainStart).count());
                                        probe_ae::onAyKick();
                                        probe_ae::setRdram(m_rdram);
                                        probe_ae::onAyChainStart(tagsProcessed, aySpans);
                                    }
                                    probe_ae::setRdram(m_rdram);
                                    probe_ae::onBbChainStart(initialTagAddr, tagsProcessed, aySpans);
                                }

                                if (isVif1 && s_fkOn && !chainBuf.empty())
                                    ps2_fk_setSpans(static_cast<uint32_t>(chainBuf.size()), aySpans);

                                chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                                chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                                if (aw3Track) // PROBE-AW3
                                    aw3RecordChain('B', initialTagAddr, static_cast<uint32_t>(tagsProcessed), aw3EndReason, tagAddr, aw3ChcrStart); // PROBE-AW3
                                return DmaChainWalkResult{tagAddr, asr0, asr1, chcr, static_cast<uint32_t>(tagsProcessed), spanCount};
                            };

                            if (!ggWalkEnabled())
                            {
                                const uint64_t wT0 = __rdtsc();
                                const DmaChainWalkResult r = walkOld(false, chcrIn, tagAddrIn, asr0In, asr1In, chainBuf);
                                t_ggLastWalkTicks = __rdtsc() - wT0;
                                return r;
                            }
                            if (ggCheckEnabled() && !t_vif1ScanOnly)
                            {
                                std::vector<uint8_t> oldBuf;
                                const DmaChainWalkResult oldRes = walkOld(true, chcrIn, tagAddrIn, asr0In, asr1In, oldBuf);
                                const uint64_t wT0 = __rdtsc();
                                const DmaChainWalkResult newRes = walkNew(chcrIn, tagAddrIn, asr0In, asr1In, chainBuf);
                                t_ggLastWalkTicks = __rdtsc() - wT0;
                                const bool match = (oldBuf.size() == chainBuf.size()) &&
                                                   (oldBuf.empty() || std::memcmp(oldBuf.data(), chainBuf.data(), oldBuf.size()) == 0) &&
                                                   oldRes.tagAddr == newRes.tagAddr &&
                                                   oldRes.asr0 == newRes.asr0 &&
                                                   oldRes.asr1 == newRes.asr1 &&
                                                   oldRes.chcr == newRes.chcr;
                                if (!match)
                                    g_ggMismatches.fetch_add(1, std::memory_order_relaxed);
                                return newRes;
                            }
                            const uint64_t wT0 = __rdtsc();
                            const DmaChainWalkResult r = walkNew(chcrIn, tagAddrIn, asr0In, asr1In, chainBuf);
                            t_ggLastWalkTicks = __rdtsc() - wT0;
                            return r;
                        };
                    }

                    const uint32_t tagAddr = m_ioRegisters[channelBase + 0x30]; // PERF-GC
                    const uint32_t asr0 = m_ioRegisters[channelBase + 0x40];    // PERF-GC
                    const uint32_t asr1 = m_ioRegisters[channelBase + 0x50];    // PERF-GC
                    const bool hasSprRef = isScratchpad(tagAddr) || isScratchpad(asr0) || isScratchpad(asr1); // PERF-GC
                    if (channelBase == 0x10009000u && vif1AsyncEnabled() && dma1WorkerCopyEnabled()) // PERF-GC
                    {
                        static uint64_t s_gcWorker = 0, s_gcSprInit = 0, s_gcSprScan = 0;
                        static auto s_gcLast = std::chrono::steady_clock::now();
                        bool useWorker = !hasSprRef;
                        if (hasSprRef)
                            ++s_gcSprInit;
                        else
                        {
                            t_vif1ScanOnly = true;
                            t_vif1ScanHitSpr = false;
                            std::vector<uint8_t> scanDummy;
                            const uint64_t pT0 = __rdtsc(); // PERF-GG
                            s_vif1WalkChain(channelBase, chcr, tagAddr, asr0, asr1, scanDummy);
                            const uint64_t pTicks = __rdtsc() - pT0; // PERF-GG
                            t_vif1ScanOnly = false;
                            if (dma1CopierActive()) // PERF-GG
                            {
                                Vif1AsyncState &st = vifAsync();
                                st.copierPrescanTicks.fetch_add(pTicks, std::memory_order_relaxed);
                                st.copierPrescanChains.fetch_add(1, std::memory_order_relaxed);
                            }
                            if (t_vif1ScanHitSpr)
                            {
                                useWorker = false;
                                ++s_gcSprScan;
                            }
                            else
                                ++s_gcWorker;
                        }
                        const auto gcNow = std::chrono::steady_clock::now();
                        if (gcNow - s_gcLast >= std::chrono::seconds(5))
                        {
                            s_gcLast = gcNow;
                            ps2xSafeLog("[gc] worker=%llu spr_init=%llu spr_scan=%llu\n",
                                        (unsigned long long)s_gcWorker, (unsigned long long)s_gcSprInit, (unsigned long long)s_gcSprScan);
                        }
                        if (useWorker)
                        {
                            processPendingTransfers();
                            g_gsFrameKicks.fetch_add(1, std::memory_order_release);
                            vif1EnqueueSourceChainJob(tagAddr, chcr, asr0, asr1, madr, qwc);
                            return true;
                        }
                    }

                    std::vector<uint8_t> chainBuf = takeVifBuf(); // PERF-VP
                    const DmaChainWalkResult res = s_vif1WalkChain(channelBase, chcr, tagAddr, asr0, asr1, chainBuf); // PERF-GC
                    m_ioRegisters[channelBase + 0x30] = res.tagAddr;
                    m_ioRegisters[channelBase + 0x40] = res.asr0;
                    m_ioRegisters[channelBase + 0x50] = res.asr1;
                    m_ioRegisters[channelBase + 0x00] = res.chcr;

                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                            g_gsFrameKicks.fetch_add(1, std::memory_order_release);
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    {
        // Task FS: log every scratchpad DMA (the normal-mode path only handles MOD=0)
        static std::atomic<uint32_t> s_fsCount{0};
        static std::atomic<uint32_t> s_fsNonNormal{0};
        if (mode != 0u)
            s_fsNonNormal.fetch_add(1, std::memory_order_relaxed);
        if (s_fsCount.fetch_add(1, std::memory_order_relaxed) < 80u || (mode != 0u && s_fsNonNormal.load(std::memory_order_relaxed) < 40u))
        {
            std::printf("[probe] fs spr dma ch=%s chcr=0x%08X mode=%u madr=0x%08X tadr=0x%08X sadr=0x%08X qwc=%u handled=%s\n",
                        channelBase == 0x1000D000u ? "fromSPR" : (channelBase == 0x1000D400u ? "toSPR" : "other"), chcr, mode,
                        m_ioRegisters[channelBase + 0x10u], m_ioRegisters[channelBase + 0x30u], m_ioRegisters[channelBase + 0x80u],
                        m_ioRegisters[channelBase + 0x20u] & 0xFFFFu, mode == 0u ? "normal" : "NO(returns false)");
            std::fflush(stdout);
        }
    }
    if (mode == 1u)
    {
        // Task FT: scratchpad chain mode.
        //  fromSPR (ch8, destination chain): tags live in SPR at SADR; each tag = {QWC, ID, ADDR=dest in main memory};
        //                                    QWC qwords follow the tag in SPR and go to ADDR. ID 7 (end) stops.
        //  toSPR   (ch9, source chain):      tags live in main memory at TADR (cnt/next/ref/refs/refe/end); data goes to SPR at SADR.
        const bool fromSpr = channelBase == kSprFromChannel;
        uint32_t sadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;
        uint32_t tadr = m_ioRegisters[channelBase + 0x30u];
        auto sprWrap = [](uint32_t v) { return v & (PS2_SCRATCHPAD_SIZE - 1u); };
        auto sprRead32 = [&](uint32_t off) { uint32_t v = 0u; std::memcpy(&v, m_scratchpad + sprWrap(off), sizeof(v)); return v; };
        uint32_t lastMadr = m_ioRegisters[channelBase + 0x10u];
        for (uint32_t tagIndex = 0; tagIndex < 4096u; ++tagIndex)
        {
            uint32_t tagLo = 0u, tagAddr = 0u;
            if (fromSpr)
            {
                tagLo = sprRead32(sadr);
                tagAddr = sprRead32(sadr + 4u);
                sadr = sprWrap(sadr + 16u);
            }
            else
            {
                uint32_t tagOff = 0u;
                try { tagOff = translateAddress(tadr); }
                catch (const std::exception &) { break; }
                if (tagOff + 16u > PS2_RAM_SIZE) break;
                std::memcpy(&tagLo, m_rdram + tagOff, 4);
                std::memcpy(&tagAddr, m_rdram + tagOff + 4, 4);
            }
            const uint32_t tagQwc = tagLo & 0xFFFFu;
            const uint32_t tagId = (tagLo >> 28) & 0x7u;
            const uint32_t bytes = tagQwc * 16u;
            {
                static std::atomic<uint32_t> s_ftTags{0};
                if (s_ftTags.fetch_add(1, std::memory_order_relaxed) < 60u)
                {
                    std::printf("[probe] ft spr chain ch=%s tag=%08X addr=%08X id=%u qwc=%u madr_reg=0x%08X sadr=0x%X tadr=0x%08X\n",
                                fromSpr ? "fromSPR" : "toSPR", tagLo, tagAddr, tagId, tagQwc,
                                m_ioRegisters[channelBase + 0x10u], sadr, tadr);
                    std::fflush(stdout);
                }
            }
            uint32_t mainAddr = 0u;   // main-memory address of the data
            uint32_t nextTadr = tadr + 16u;
            if (fromSpr)
            {
                mainAddr = tagAddr & 0x7FFFFFF0u;
            }
            else
            {
                switch (tagId)
                {
                case 0: mainAddr = tagAddr & 0x7FFFFFF0u; break;                       // refe
                case 1: mainAddr = tadr + 16u; nextTadr = mainAddr + bytes; break;     // cnt
                case 2: mainAddr = tadr + 16u; nextTadr = tagAddr & 0x7FFFFFF0u; break; // next
                case 3:
                case 4: mainAddr = tagAddr & 0x7FFFFFF0u; break;                       // ref / refs
                case 7: mainAddr = tadr + 16u; break;                                  // end
                default: mainAddr = tadr + 16u; nextTadr = mainAddr + bytes; break;    // call/ret unsupported: treat as cnt
                }
            }
            if (bytes != 0u)
            {
                uint32_t mainOff = 0u;
                try { mainOff = translateAddress(mainAddr); }
                catch (const std::exception &) { break; }
                if (mainOff > PS2_RAM_SIZE || bytes > PS2_RAM_SIZE - mainOff) break;
                uint32_t done = 0u;
                while (done < bytes)
                {
                    const uint32_t chunk = std::min(bytes - done, PS2_SCRATCHPAD_SIZE - sprWrap(sadr + done));
                    if (fromSpr)
                    {
                        std::memcpy(m_rdram + mainOff + done, m_scratchpad + sprWrap(sadr + done), chunk);
                        markModified(mainOff + done, chunk);
                    }
                    else
                    {
                        std::memcpy(m_scratchpad + sprWrap(sadr + done), m_rdram + mainOff + done, chunk);
                    }
                    done += chunk;
                }
                lastMadr = mainAddr + bytes;
                sadr = sprWrap(sadr + bytes);
            }
            if (fromSpr)
            {
                if (tagId == 7u) break;
            }
            else
            {
                tadr = nextTadr;
                if (tagId == 7u || tagId == 0u) break;
            }
        }
        m_ioRegisters[channelBase + 0x10u] = lastMadr & 0x7FFFFFF0u;
        m_ioRegisters[channelBase + 0x20u] = 0u;
        m_ioRegisters[channelBase + 0x80u] = sadr & 0x3FF0u;
        if (!fromSpr)
            m_ioRegisters[channelBase + 0x30u] = tadr;
        completeDmacChannel(channelBase, fromSpr ? 8u : 9u);
        return true;
    }
    if (mode != 0u)
        return false;

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::processPendingTransfers()
{
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
    }

    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();
    const bool vifAsync = vif1AsyncEnabled(); // PERF-HN
    if (vifAsync && hadGif && m_gifArbiter)
        m_gifArbiter->drain(); // worker is idle here (submitGifPacket fenced); flush PATH3 before VIF1 jobs are queued

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    for (auto &p : m_pendingVif1Transfers)
    {
        if (vifAsync) // PERF-HN: copy the packet data now (the EE may reuse the buffers), process on the worker
        {
            if (!s_vif1Process)
            {
                s_vif1DrainArbiter = [this]() { if (m_gifArbiter) m_gifArbiter->drain(); };
                s_vif1Process = [this](const uint8_t *d, uint32_t n) { processVIF1Data(d, n); };
            }
            if (!p.chainData.empty())
            {
                vif1EnqueueJob(std::move(p.chainData));
            }
            else if (p.qwc > 0)
            {
                uint32_t srcPhys = 0;
                const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
                uint32_t bytesLeft = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                try
                {
                    srcPhys = translateAddress(p.srcAddr);
                }
                catch (const std::exception &)
                {
                    continue;
                }
                const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= limit)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > limit)
                        chunk = limit - srcPhys;
                    if (chunk == 0)
                        break;
                    vif1EnqueueCopy(base + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            continue;
        }
        if (!p.chainData.empty())
        {
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();

    if (m_gifArbiter && !vifAsync)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif || hadVif0 || hadVif1) // PERF-GC
        syncDma1PublishedToIoRegs(m_ioRegisters);

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

namespace {
    std::vector<uint32_t> s_completedIntcCauses;
    std::mutex s_completedIntcMutex;
}

void recordCompletedIntcCause(uint32_t cause)
{
    static const bool s_aw2Probe = []() { const char *e = std::getenv("PS2X_AW2_PROBE"); return e && e[0] == '1'; }(); // PROBE-AW2
    if (s_aw2Probe && cause == 5u) g_aw2C5Raised.fetch_add(1, std::memory_order_relaxed); // PROBE-AW2
    std::lock_guard<std::mutex> lock(s_completedIntcMutex);
    s_completedIntcCauses.push_back(cause);
}

std::vector<uint32_t> consumeCompletedIntcCauses()
{
    std::lock_guard<std::mutex> lock(s_completedIntcMutex);
    std::vector<uint32_t> causes;
    causes.swap(s_completedIntcCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;
    ps2_vif1_fence(); // PERF-HN: EE-thread submits must not overtake / race the VIF1 worker's PATH1/PATH2 packets

    probe_ae::onBhSubmitGifStart();
    // PERF-KK: the timestamp is only read by the probe_ae block at the end of this function
    const auto tGifStart = probe_ae::isEnabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            probe_ae::onBhSubmitGifEnd();
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();

#if defined(PS2X_PROBE_AE) || 1
    if (probe_ae::isEnabled()) {
        auto tGifEnd = std::chrono::steady_clock::now();
        probe_ae::onAyTimeGif(std::chrono::duration_cast<std::chrono::microseconds>(tGifEnd - tGifStart).count());
    }
#endif
    probe_ae::onBhSubmitGifEnd();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    ps2_vif1_fence(); // PERF-HN
    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    static uint32_t s_uploadChainSeq = 0;
    ++s_uploadChainSeq;
    uint32_t dbp = static_cast<uint32_t>(setupRegs[0] & 0x3FFFull);
    uint32_t dpsm = static_cast<uint32_t>((setupRegs[0] >> 24u) & 0x3Full);
    uint32_t rrw = static_cast<uint32_t>(setupRegs[2] & 0xFFFull);
    uint32_t rrh = static_cast<uint32_t>((setupRegs[2] >> 32u) & 0xFFFull);
    std::fprintf(stderr, "[GIF:DMA_SRC] seq=%u guestAddr=0x%08X bytes=%u tagId=%u tagAddr=0x%08X dbp=%u dpsm=0x%02X (%ux%u)\n",
                 s_uploadChainSeq, imageDataAddr, imageBytes, payloadTag.id, payloadTagAddr, dbp, dpsm, rrw, rrh);
    if (imageData && imageBytes >= 16)
    {
        std::fprintf(stderr, "[GIF:DMA_SRC_DATA] first16=[%u,%u,%u,%u,%u,%u,%u,%u, %u,%u,%u,%u,%u,%u,%u,%u]\n",
                     imageData[0], imageData[1], imageData[2], imageData[3],
                     imageData[4], imageData[5], imageData[6], imageData[7],
                     imageData[8], imageData[9], imageData[10], imageData[11],
                     imageData[12], imageData[13], imageData[14], imageData[15]);
    }
    std::fflush(stderr);

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    ps2_gs_async_fence(); // PERF-GK: keep order vs queued GIF packets
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    ps2_vif1_fence(); // PERF-HN
    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    ps2_gs_async_fence(); // PERF-GK
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        ps2_vif1_fence(); // PERF-HN (m_path3Masked)
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               (m_path3Masked ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address == 0x10003C00u) // VIF1_STAT
    {
        ps2_vif1_fence(); // PERF-HN
        if (g_vif1PendingStc.exchange(false, std::memory_order_acq_rel)) // PERF-V1K
            vif1_regs.stat &= ~kVif1StcClearMask;
        return vif1_regs.stat;
    }

    if (address == 0x10003C30u) // VIF1_MARK
    {
        if (!t_onVif1Worker && markNoFenceEnabled()) // PERF-V1K
            return g_vif1PublishedMark.load(std::memory_order_acquire);
        ps2_vif1_fence(); // PERF-HN
        return vif1_regs.mark;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (!t_onVif1Worker && ((address & 0xFFFFFF00u) == 0x10009000u || address == 0x1000E010u)) // PERF-GC
        {
            waitDma1CopyDone();
            syncDma1PublishedToIoRegs(m_ioRegisters);
        }

        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                if (address == 0x10009000u) {
                    probe_ae::onBhDmaPollTime(1);
                }
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
