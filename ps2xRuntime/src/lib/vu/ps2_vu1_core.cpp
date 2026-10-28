#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#endif
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "probe_ae.h"
#include "vu1_native_api.h"

#include <chrono>
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <ps2_log.h>

// =========================================================================
// Task FE: VU1 data writer tagging (UNPACK & VUSTORE) for qw 636..698 / 716..778
// =========================================================================
enum class FeWriterKind : uint8_t {
    NONE = 0,
    UNPACK = 1,
    VUSTORE = 2
};

struct FeWriterTag {
    FeWriterKind kind = FeWriterKind::NONE;
    uint32_t ev = 0;
    uint8_t cmd = 0;
    uint16_t dest = 0;
    uint16_t num = 0;
    uint8_t cl = 0;
    uint8_t wl = 0;
    uint8_t flg = 0;
    uint16_t tops = 0;
    uint32_t src_ea = 0;
    uint32_t pc = 0;
};

static std::mutex s_feWriterMutex;
static FeWriterTag s_feWriter[1024];

extern "C" void ps2_fe_recordUnpack(uint32_t qw, uint32_t ev, uint8_t cmd, uint16_t dest, uint16_t num,
                                    uint8_t cl, uint8_t wl, uint8_t flg, uint16_t tops, uint32_t src_ea)
{
    if (qw < 1024u)
    {
        std::lock_guard<std::mutex> lock(s_feWriterMutex);
        auto &w = s_feWriter[qw];
        w.kind = FeWriterKind::UNPACK;
        w.ev = ev;
        w.cmd = cmd;
        w.dest = dest;
        w.num = num;
        w.cl = cl;
        w.wl = wl;
        w.flg = flg;
        w.tops = tops;
        w.src_ea = src_ea;
        w.pc = 0;
    }
}

extern "C" void ps2_fe_recordVuStore(uint32_t qw, uint32_t pc)
{
    if (qw < 1024u)
    {
        std::lock_guard<std::mutex> lock(s_feWriterMutex);
        auto &w = s_feWriter[qw];
        w.kind = FeWriterKind::VUSTORE;
        w.pc = pc;
    }
}

extern "C" void ps2x_probeEvent(const char *msg);
extern "C" void ps2x_probeDump();
extern "C" uint32_t ps2_vif1_getEuMatchedCount();
extern "C" void ps2_vif1_dumpEvBuffers();
extern "C" void ps2_fe_recordVuStore(uint32_t qw, uint32_t pc);
static thread_local bool s_euStoreLoggedThisRun = false;

// =========================================================================
// Task DY: VU1 program execution & cycle cap telemetry
// =========================================================================
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>

namespace probe_ae {
    bool isDcProbeEnabled();
}

struct PcStats {
    uint32_t calls = 0;
    uint32_t hitCap = 0;
    uint64_t totalCycles = 0;
    uint64_t totalInstrs = 0;
};

static std::mutex s_dyMutex;
static uint32_t s_dyCalls = 0;
static uint32_t s_dyMscnt = 0;
static uint32_t s_dyEndedEbit = 0;
static uint32_t s_dyHitCap = 0;
static uint32_t s_dyOther = 0;
static uint64_t s_dyTotalInstrs = 0;
static uint64_t s_dyTotalCycles = 0;
static uint32_t s_dyMaxInstrs = 0;
static uint32_t s_dyMaxCycles = 0;
static std::unordered_map<uint32_t, PcStats> s_dyPcStats;
static uint32_t s_dySecCount = 0;
std::atomic<uint32_t> g_diagMscalCount{0};

static uint32_t s_dyLastPcs[4096];
static uint32_t s_dyLastPcIdx = 0;
static bool s_dyCappedDumped = false;

static thread_local bool s_dyCurIsMscnt = false;
static thread_local uint32_t s_dyCurStartPc = 0;
static thread_local uint32_t s_dyCurTop = 0;
static thread_local uint32_t s_dyCurItop = 0;

// =========================================================================
// Task ED: VU1 3D Census (PS2X_PROBE_VU3D=1)
// =========================================================================
extern "C" void ps2_vif1_dumpDzRingCount(uint32_t maxCount, const char *prefix);
static bool s_eeVuDone = false;
struct EePacket {
    uint32_t qwCount = 0;
    uint32_t words[64][4];
};
static thread_local std::vector<EePacket> s_eeCurRunPackets;

static inline bool isVu3dProbeEnabled()
{
    static int s_vu3d = -1;
    if (s_vu3d == -1)
    {
        const char *v = std::getenv("PS2X_PROBE_VU3D");
        s_vu3d = (v && (v[0] == '1' || v[0] == 't')) ? 1 : 0;
    }
    return s_vu3d == 1;
}

struct VdRunStats {
    uint32_t runs = 0;
    uint32_t capped = 0;
    uint64_t totalCycles = 0;
    uint32_t kicks = 0;
    uint32_t verts = 0;
    uint32_t vZeroXy = 0;
    uint32_t vOob = 0;
    uint32_t primHist[7] = {0}; // 0:point, 1:line, 2:linestrip, 3:tri, 4:strip, 5:fan, 6:sprite
};

static std::mutex s_vdMutex;
static std::unordered_map<uint32_t, VdRunStats> s_vdStats;
static std::chrono::steady_clock::time_point s_vdLastWindowTime{};
static const auto s_vdStartTime = std::chrono::steady_clock::now();

// =========================================================================
// Task EG: Instruction trace of bad VU1 pc=0x08 run
// =========================================================================
struct EgVfChange {
    uint8_t reg;
    uint32_t words[4];
};
struct EgViChange {
    uint8_t reg;
    int32_t val;
};
struct EgPairTrace {
    uint32_t pc;
    uint32_t upper;
    uint32_t lower;
    uint8_t numVf;
    uint8_t numVi;
    EgVfChange vfChanges[4];
    EgViChange viChanges[4];
    uint32_t qHex;
    uint32_t mac;
    uint32_t status;
    uint32_t clip;
    uint32_t pHex;
};

static EgPairTrace s_egTraces[700];
static uint32_t s_egTraceCount = 0;
static uint32_t s_egPc08RunCount = 0;
static bool s_egCurRunTraced = false;
static bool s_egDone = false;
static float s_egEntryVf[32][4];
static int32_t s_egEntryVi[16];
static uint32_t s_egEntryTop = 0;
static uint32_t s_egEntryItop = 0;
static uint32_t s_egEntryDataWords[281][4]; // qw 440..720 (281 qw)
static uint32_t s_egTotalPairs = 0;
static std::vector<EePacket> s_egCurRunPackets;

// =========================================================================
// Task EN: VU1 store detection into qw 970..1011 & 5s count reporting
// =========================================================================
extern "C" uint32_t ps2_vif1_getEoUnpackBadCount();
static uint32_t s_eoVuStoreBadCount = 0;
static uint32_t s_eoVuStorePrintCount = 0;
static std::chrono::steady_clock::time_point s_eoLastCountTime{};

// =========================================================================
// Task FD: Full instruction trace of draw run <C80,070,1EE0,000> with >=20 zero_xy
// =========================================================================
static const char *getLowerOpName(uint32_t lower)
{
    if (lower == 0u || lower == 0x8000033Cu) return "NOP";
    uint32_t opHi = (lower >> 25) & 0x7Fu;
    switch (opHi)
    {
    case 0x00: return "LQ";
    case 0x01: return "SQ";
    case 0x04: return "ILW";
    case 0x05: return "ISW";
    case 0x08: return "IADDIU";
    case 0x09: return "ISUBIU";
    case 0x10: return "FCEQ";
    case 0x11: return "FCSET";
    case 0x12: return "FCAND";
    case 0x13: return "FCOR";
    case 0x14: return "FSEQ";
    case 0x15: return "FSSET";
    case 0x16: return "FSAND";
    case 0x17: return "FSOR";
    case 0x18: return "FMEQ";
    case 0x1A: return "FMAND";
    case 0x1B: return "FMOR";
    case 0x1C: return "FCGET";
    case 0x20: return "B";
    case 0x21: return "BAL";
    case 0x24: return "JR";
    case 0x25: return "JALR";
    case 0x28: return "IBEQ";
    case 0x29: return "IBNE";
    case 0x2C: return "IBLTZ";
    case 0x2D: return "IBGTZ";
    case 0x2E: return "IBLEZ";
    case 0x2F: return "IBGEZ";
    case 0x40:
    default:
        break;
    }

    if (opHi >= 0x40u || (lower & 0x80000000u))
    {
        uint32_t opLo = lower & 0x3Fu;
        if (opLo < 0x3Cu)
        {
            switch (opLo)
            {
            case 0x30: return "IADD";
            case 0x31: return "ISUB";
            case 0x32: return "IADDI";
            case 0x34: return "IAND";
            case 0x35: return "IOR";
            default: break;
            }
        }
        else
        {
            const uint8_t funct2 = static_cast<uint8_t>((lower & 0x3u) | ((lower >> 4) & 0x7Cu));
            switch (funct2)
            {
            case 0x30: return "MOVE";
            case 0x31: return "MR32";
            case 0x34: return "LQI";
            case 0x35: return "SQI";
            case 0x36: return "LQD";
            case 0x37: return "SQD";
            case 0x38: return "DIV";
            case 0x39: return "SQRT";
            case 0x3A: return "RSQRT";
            case 0x3B: return "WAITQ";
            case 0x3C: return "MTIR";
            case 0x3D: return "MFIR";
            case 0x3E: return "ILWR";
            case 0x3F: return "ISWR";
            case 0x40: return "RNEXT";
            case 0x41: return "RGET";
            case 0x42: return "RINIT";
            case 0x43: return "RXOR";
            case 0x64: return "MFP";
            case 0x68: return "XTOP";
            case 0x69: return "XITOP";
            case 0x6C: return "XGKICK";
            case 0x70: return "ESADD";
            case 0x71: return "ERSADD";
            case 0x72: return "ELENG";
            case 0x73: return "ERLENG";
            case 0x74: return "EATANxy";
            case 0x75: return "EATANxz";
            case 0x76: return "ESUM";
            case 0x77: return "ERSQRT";
            case 0x78: return "ESQRT";
            case 0x79: return "ESIN";
            case 0x7A: return "ERCPR";
            case 0x7B: return "WAITP";
            case 0x7C: return "EATAN";
            case 0x7D: return "EEXP";
            default: break;
            }
        }
    }

    return "OTHER";
}

static EgPairTrace s_emFirstTraces[1000];
static EgPairTrace s_emTailRing[1000];
static uint32_t s_emTraceCount = 0;
static bool s_emDone = false;
static uint32_t s_fwSkip = 0;
static bool s_emCurRunCandidate = false;
static bool s_emRecording = false;
static uint32_t s_emCurRunPairs = 0;
static uint32_t s_emLastPairPc = 0xFFFFFFFFu;
static std::vector<uint32_t> s_emCurRunTargets;
static float s_emEntryVf[32][4];
static int32_t s_emEntryVi[16];
static uint32_t s_emEntryTop = 0;
static uint32_t s_emEntryItop = 0;
static float s_emEntryQ = 0.0f;
static float s_emEntryP = 0.0f;
static uint32_t s_emEntryVuData[1024][4];
static std::vector<EePacket> s_emCurRunPackets;
static std::map<std::string, uint32_t> s_emLowOpHist;

// =========================================================================
// Task EH: VU1 store recording & dumps
// =========================================================================
extern "C" void ps2_vif1_dumpEhRing();
extern "C" void ps2_vif1_dumpEhShapes();

struct EhStoreRecord {
    uint32_t pair;
    uint32_t pc;
    uint32_t qw;
    uint32_t words[4];
    const char *kind;
};
static EhStoreRecord s_ehStores[1024];
static uint32_t s_ehStoreCount = 0;
static thread_local uint32_t s_ehCurLowerInstr = 0;

// =========================================================================
// Task EK: VU1 program 0x08 command-sequence census
// =========================================================================
struct EkSeqStats {
    uint32_t runs = 0;
    uint64_t totalPairs = 0;
    uint64_t verts = 0;
    uint64_t zeroXy = 0;
    uint64_t st636 = 0;
    uint64_t st636Zero = 0;
};
struct EkOtherStats {
    uint32_t runs = 0;
    uint64_t cycles = 0;
};

static std::unordered_map<std::string, EkSeqStats> s_ekSeqStats;
static std::map<uint32_t, uint32_t> s_ekCmdHist;
static std::map<uint32_t, EkOtherStats> s_ekOtherStats;
static std::set<std::string> s_ekSeenHdrSequences;

static thread_local bool s_ekCurRunActive = false;
static thread_local uint32_t s_ekCurRunPairs = 0;
static thread_local uint32_t s_ekCurRunSt636 = 0;
static thread_local uint32_t s_ekCurRunSt636Zero = 0;
static thread_local uint32_t s_ekLastPairPc = 0xFFFFFFFFu;
static thread_local std::vector<uint32_t> s_ekCurRunTargets;
static thread_local uint32_t s_ekEntryVuData[1024][4];

// =========================================================================
// Task EL: Run order / staleness tracking for qw 636..699
// =========================================================================
struct ElLastWriter {
    bool written = false;
    uint64_t runId = 0;
    uint32_t startPc = 0;
    uint32_t pairIdx = 0;
    bool allZero = false;
};

struct ElDrawHdr {
    uint64_t runId = 0;
    uint32_t top = 0;
    uint32_t words[12][4]{};
};

static uint64_t s_elGlobalRunId = 0;
static ElLastWriter s_elLastWriter[64];
static bool s_elWindowOpened = false;
static uint32_t s_elWindowRunsRemaining = 0;
static bool s_elHdrsDumped = false;
static std::vector<ElDrawHdr> s_elDrawHdrs;

static thread_local uint64_t s_elCurRunId = 0;
static thread_local uint32_t s_elCurRunPairs = 0;
static thread_local uint32_t s_elCurRunStCount = 0;
static thread_local uint32_t s_elCurRunStMinQw = 9999;
static thread_local uint32_t s_elCurRunStMaxQw = 0;
static thread_local uint32_t s_elCurRunRdCount = 0;
static thread_local uint32_t s_elCurRunRdMinQw = 9999;
static thread_local uint32_t s_elCurRunRdMaxQw = 0;
static thread_local uint32_t s_elCurRunRdZero = 0;
static thread_local uint32_t s_elCurRunRdNever = 0;
static thread_local uint64_t s_elCurRunRdMinAge = 0xFFFFFFFFFFFFFFFFULL;
static thread_local uint64_t s_elCurRunRdMaxAge = 0;
static thread_local bool s_elCurRunHaveAge = false;

// =========================================================================
// Task EJ: No time gate + last-writer table for qw 636..700 + ei read trigger
// =========================================================================
static inline uint32_t getVu3dAfterSec() {
    static int s_after = -1;
    if (s_after == -1) {
        const char *v = std::getenv("PS2X_VU3D_AFTER_S");
        s_after = v ? std::atoi(v) : 0;
        if (s_after < 0) s_after = 0;
    }
    return static_cast<uint32_t>(s_after);
}

struct EiPairRingEntry {
    uint32_t pc = 0;
    uint32_t upper = 0;
    uint32_t lower = 0;
    int32_t mem_addr_qw = -1;
    uint32_t mem_first_lane_u32 = 0;
    bool mem_is_store = false;
};

struct EiRunHistory {
    uint32_t runIndex = 0;
    uint32_t startPc = 0;
    uint64_t cycles = 0;
    uint32_t st636 = 0;
    uint32_t garb = 0;
    std::vector<uint32_t> disp;
};

struct EiLastWriter {
    bool written = false;
    uint32_t pc = 0;
    uint32_t run = 0;
    uint32_t pair = 0;
    uint32_t startPc = 0;
    uint32_t words[4] = {0};
};

static bool s_eiHitDone = false;
static bool s_eiReadDone = false;
static uint32_t s_eiTotalRuns = 0;
static uint32_t s_eiCurRunPairs = 0;
static uint32_t s_eiCurRunSt636 = 0;
static uint32_t s_eiCurRunGarb = 0;
static uint32_t s_eiLastPairPc = 0xFFFFFFFFu;
static std::vector<uint32_t> s_eiCurRunDisp;
static uint32_t s_eiCurStartPc = 0;
static uint32_t s_eiCurTop = 0;
static uint32_t s_eiCurItop = 0;
static uint64_t s_eiRunStartCycle = 0;
static thread_local bool s_eiCurRunActive = false;
static uint32_t s_eiEntryVuData[1024][4];

static EiLastWriter s_eiLastWriter[64];

static EiPairRingEntry s_eiRing[160];
static uint32_t s_eiRingHead = 0;
static uint32_t s_eiRingCount = 0;

static EiRunHistory s_eiRunHist[12];
static uint32_t s_eiRunHistHead = 0;
static uint32_t s_eiRunHistCount = 0;

struct EiMemOp {
    int32_t qw = -1;
    uint32_t firstLane = 0;
    bool isStore = false;
};

static inline EiMemOp decodeEiMem(uint32_t lower, const VU1State &st, const uint8_t *vuData, uint32_t dataSize)
{
    EiMemOp op;
    uint32_t opHi = (lower >> 25) & 0x7Fu;
    auto getImm11 = [](uint32_t ins) -> int16_t {
        uint16_t v = ins & 0x7FFu;
        if (v & 0x400u) v |= 0xF800u;
        return static_cast<int16_t>(v);
    };
    uint8_t fs = (lower >> 11) & 0x1Fu;
    uint8_t it = (lower >> 16) & 0x0Fu;
    uint8_t is = (lower >> 11) & 0x0Fu;

    if (opHi == 0x00u) { // LQ
        int32_t q = (static_cast<int32_t>(st.vi[is]) + getImm11(lower)) & 0x3FF;
        op.qw = q;
        op.isStore = false;
        if (vuData && (q + 1) * 16 <= static_cast<int32_t>(dataSize)) {
            std::memcpy(&op.firstLane, vuData + q * 16, 4);
        }
    } else if (opHi == 0x01u) { // SQ
        int32_t q = (static_cast<int32_t>(st.vi[it]) + getImm11(lower)) & 0x3FF;
        op.qw = q;
        op.isStore = true;
        std::memcpy(&op.firstLane, &st.vf[fs][0], 4);
    } else if (opHi == 0x04u) { // ILW
        int32_t q = (static_cast<int32_t>(st.vi[is]) + getImm11(lower)) & 0x3FF;
        op.qw = q;
        op.isStore = false;
        if (vuData && (q + 1) * 16 <= static_cast<int32_t>(dataSize)) {
            std::memcpy(&op.firstLane, vuData + q * 16, 4);
        }
    } else if (opHi == 0x05u) { // ISW
        int32_t q = (static_cast<int32_t>(st.vi[is]) + getImm11(lower)) & 0x3FF;
        op.qw = q;
        op.isStore = true;
        op.firstLane = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it] & 0xFFFF));
    } else if (opHi >= 0x40u || (lower & 0x80000000u)) {
        uint32_t special = (lower & 3u) | ((lower >> 4) & 0x7Cu);
        if (special == 0x34u) { // LQI
            int32_t q = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[is]))) & 0x3FF;
            op.qw = q;
            op.isStore = false;
            if (vuData && (q + 1) * 16 <= static_cast<int32_t>(dataSize)) {
                std::memcpy(&op.firstLane, vuData + q * 16, 4);
            }
        } else if (special == 0x35u) { // SQI
            int32_t q = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it]))) & 0x3FF;
            op.qw = q;
            op.isStore = true;
            std::memcpy(&op.firstLane, &st.vf[fs][0], 4);
        } else if (special == 0x36u) { // LQD
            int32_t q = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[is] - 1))) & 0x3FF;
            op.qw = q;
            op.isStore = false;
            if (vuData && (q + 1) * 16 <= static_cast<int32_t>(dataSize)) {
                std::memcpy(&op.firstLane, vuData + q * 16, 4);
            }
        } else if (special == 0x37u) { // SQD
            int32_t q = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it] - 1))) & 0x3FF;
            op.qw = q;
            op.isStore = true;
            std::memcpy(&op.firstLane, &st.vf[fs][0], 4);
        } else if (special == 0x3Eu) { // ILWR
            int32_t q = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[is]))) & 0x3FF;
            op.qw = q;
            op.isStore = false;
            if (vuData && (q + 1) * 16 <= static_cast<int32_t>(dataSize)) {
                std::memcpy(&op.firstLane, vuData + q * 16, 4);
            }
        } else if (special == 0x3Fu) { // ISWR
            int32_t q = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[is]))) & 0x3FF;
            op.qw = q;
            op.isStore = true;
            op.firstLane = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it] & 0xFFFF));
        }
    }
    return op;
}

static void dumpEiDiagnostics(const char *triggerName, uint32_t pc, uint32_t qw, const uint32_t words[4], uint32_t procSec, bool isRead, const VU1State &st, uint64_t curCycle)
{
    if (isRead)
    {
        std::printf("[probe] ei read run=%u sec=%u pair=%u pc=0x%03X qw=%u w=%08X %08X %08X %08X\r\n",
                    s_eiTotalRuns, procSec, s_eiCurRunPairs, pc, qw, words[0], words[1], words[2], words[3]);
    }
    else
    {
        std::printf("[probe] ei hit run=%u sec=%u pair=%u pc=0x%03X qw=%u w=%08X %08X %08X %08X\r\n",
                    s_eiTotalRuns, procSec, s_eiCurRunPairs, pc, qw, words[0], words[1], words[2], words[3]);
    }
    if (isRead)
    {
        if (qw >= 636u && qw < 700u && s_eiLastWriter[qw - 636u].written)
        {
            const auto &lw = s_eiLastWriter[qw - 636u];
            std::printf("[probe] ei lastwriter qw=%u pc=0x%03X run=%u pair=%u start=0x%03X w=%08X %08X %08X %08X\n",
                        qw, lw.pc, lw.run, lw.pair, lw.startPc, lw.words[0], lw.words[1], lw.words[2], lw.words[3]);
        }
        else
        {
            std::printf("[probe] ei lastwriter none qw=%u\n", qw);
        }
    }

    uint32_t rStart = (s_eiRingCount == 160u) ? s_eiRingHead : 0u;
    for (uint32_t i = 0; i < s_eiRingCount; ++i) {
        const auto &rp = s_eiRing[(rStart + i) % 160u];
        if (rp.mem_addr_qw >= 0) {
            std::printf("[probe] ei pc=0x%03X up=0x%08X lo=0x%08X mem=%cqw%d:%08X\n",
                        rp.pc, rp.upper, rp.lower, (rp.mem_is_store ? 'S' : 'L'), rp.mem_addr_qw, rp.mem_first_lane_u32);
        } else {
            std::printf("[probe] ei pc=0x%03X up=0x%08X lo=0x%08X\n",
                        rp.pc, rp.upper, rp.lower);
        }
    }

    for (int r = 0; r < 32; ++r) {
        uint32_t rw[4];
        std::memcpy(rw, st.vf[r], 16);
        std::printf("[probe] ei vf%02d=%08X %08X %08X %08X\n", r, rw[0], rw[1], rw[2], rw[3]);
    }
    std::string viStr;
    for (int r = 1; r < 16; ++r) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%d", (r > 1 ? " " : ""), st.vi[r]);
        viStr += buf;
    }
    uint32_t qHex = 0, pHex = 0;
    std::memcpy(&qHex, &st.q, 4);
    std::memcpy(&pHex, &st.p, 4);
    std::printf("[probe] ei vi=%s top=%u itop=%u q=0x%08X p=0x%08X\n",
                viStr.c_str(), s_eiCurTop, s_eiCurItop, qHex, pHex);

    std::set<uint32_t> loadedQws;
    for (uint32_t i = 0; i < s_eiRingCount; ++i) {
        const auto &rp = s_eiRing[(rStart + i) % 160u];
        if (!rp.mem_is_store && rp.mem_addr_qw >= 0 && rp.mem_addr_qw < 1024) {
            loadedQws.insert(static_cast<uint32_t>(rp.mem_addr_qw));
        }
    }
    for (uint32_t q : loadedQws) {
        const auto &w = s_eiEntryVuData[q];
        std::printf("[probe] ei in qw=%u w=%08X %08X %08X %08X\n", q, w[0], w[1], w[2], w[3]);
    }

    uint32_t hStart = (s_eiRunHistCount == 12u) ? s_eiRunHistHead : 0u;
    for (uint32_t h = 0; h < s_eiRunHistCount; ++h) {
        const auto &rh = s_eiRunHist[(hStart + h) % 12u];
        std::string dispStr;
        for (size_t d = 0; d < rh.disp.size(); ++d) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s0x%03X", (d > 0 ? "," : ""), rh.disp[d]);
            dispStr += buf;
        }
        std::printf("[probe] ei run n=%u start=0x%03X cyc=%llu st636=%u garb=%u disp=%s\n",
                    rh.runIndex, rh.startPc, static_cast<unsigned long long>(rh.cycles),
                    rh.st636, rh.garb, dispStr.c_str());
    }
    std::string curDispStr;
    for (size_t d = 0; d < s_eiCurRunDisp.size(); ++d) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s0x%03X", (d > 0 ? "," : ""), s_eiCurRunDisp[d]);
        curDispStr += buf;
    }
    std::printf("[probe] ei run n=%u start=0x%03X cyc=%llu st636=%u garb=%u disp=%s\n",
                s_eiTotalRuns, s_eiCurStartPc, static_cast<unsigned long long>(curCycle - s_eiRunStartCycle),
                s_eiCurRunSt636, s_eiCurRunGarb, curDispStr.c_str());

    std::fflush(stdout);
}

struct VdCandidateEntry {
    bool valid = false;
    uint32_t pc = 0;
    uint32_t top = 0;
    uint32_t vertCount = 0;
    float vf[32][4];
    int32_t vi[16];
    uint32_t dataWords[26][4]; // [top-2, top+24)
    uint32_t outWords[14][4];  // first 14 qw of first XGKICK
};
static std::map<uint32_t, VdCandidateEntry> s_vdCandidateEntries;
static std::set<uint32_t> s_vdDumpedPcs;

static thread_local uint32_t s_vdCurRunKicks = 0;
static thread_local uint32_t s_vdCurRunVerts = 0;
static thread_local uint32_t s_vdCurRunZeroXy = 0;
static thread_local uint32_t s_vdCurRunOob = 0;
static thread_local uint32_t s_vdCurRunPrimHist[7] = {0};
static thread_local uint32_t s_vdFirstXgkickWords[14][4] = {{0}};
static thread_local bool s_vdHaveFirstXgkick = false;

static void decodeXgkickPacketStats(const uint8_t *data, uint32_t size,
                                   uint32_t &outVerts, uint32_t &outZeroXy, uint32_t &outOob,
                                   uint32_t primHist[7])
{
    uint32_t offset = 0;
    while (offset + 16u <= size)
    {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, data + offset, sizeof(tagLo));
        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u) nreg = 16u;
        uint32_t primType = static_cast<uint32_t>((tagLo >> 47) & 0x7u);
        if (primType < 7u)
            primHist[primType] += nloop;
        uint64_t tagHi = 0;
        std::memcpy(&tagHi, data + offset + 8u, sizeof(tagHi));
        uint32_t tagBytes = 16u;
        if (format == 0u)
        {
            uint8_t regs[16];
            for (uint32_t r = 0; r < nreg; ++r)
                regs[r] = static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu);
            uint32_t itemOffset = offset + 16u;
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (itemOffset + 16u <= size)
                    {
                        if (regs[r] == 0x05u || regs[r] == 0x0Du) // XYZ2 (5) or XYZF2 (D)
                        {
                            outVerts++;
                            uint16_t xRaw = 0, yRaw = 0;
                            std::memcpy(&xRaw, data + itemOffset + 0u, 2);
                            std::memcpy(&yRaw, data + itemOffset + 4u, 2);
                            if (xRaw == 0 && yRaw == 0)
                                outZeroXy++;
                            if (xRaw > (4095u << 4u) || yRaw > (4095u << 4u))
                                outOob++;
                        }
                    }
                    itemOffset += 16u;
                }
            }
            tagBytes += nloop * nreg * 16u;
        }
        else if (format == 1u)
        {
            tagBytes += ((nloop * nreg + 1u) & ~1u) * 8u;
        }
        else if (format == 2u)
        {
            tagBytes += nloop * 16u;
        }
        else
        {
            break;
        }
        if (tagBytes < 16u || offset + tagBytes > size)
            break;
        offset += tagBytes;
        if (((tagLo >> 15) & 1u) != 0u)
            break;
    }
}

// Task DZ state
extern "C" void ps2_vif1_dumpDzRing();
static std::mutex s_dzMutex;
static uint32_t s_dzNormalLogged = 0;
static uint32_t s_dzCappedLogged = 0;
static bool s_dzFirstCappedDumpDone = false;
static thread_local bool s_dzPendingRun = false;
static thread_local uint32_t s_dzPendingStartPc = 0;
static thread_local uint32_t s_dzPendingTop = 0;
static thread_local uint32_t s_dzPendingItop = 0;
static thread_local int32_t s_dzPendingVi[16];
static thread_local uint32_t s_dzPendingDataWords[32][4];

extern "C" void ps2_vu1_dumpDyPerSec(const uint8_t *rdram) {
    if (!probe_ae::isDcProbeEnabled()) return;
    std::lock_guard<std::mutex> lock(s_dyMutex);
    if (s_dyCalls == 0) return;
    if (s_dySecCount >= 20) return;
    s_dySecCount++;

    std::printf("[probe] dc14 vu1 calls=%u mscnt=%u ended_ebit=%u hit_cap=%u other=%u instrs=%llu cycles=%llu max_instrs=%u max_cycles=%u\n",
                s_dyCalls, s_dyMscnt, s_dyEndedEbit, s_dyHitCap, s_dyOther,
                (unsigned long long)s_dyTotalInstrs, (unsigned long long)s_dyTotalCycles,
                s_dyMaxInstrs, s_dyMaxCycles);

    // Top 6 PCs by total cycles
    std::vector<std::pair<uint32_t, PcStats>> pcVec(s_dyPcStats.begin(), s_dyPcStats.end());
    std::sort(pcVec.begin(), pcVec.end(), [](const auto &a, const auto &b) {
        return a.second.totalCycles > b.second.totalCycles;
    });

    size_t limit = std::min<size_t>(6u, pcVec.size());
    for (size_t i = 0; i < limit; ++i) {
        const auto &p = pcVec[i];
        uint64_t avgC = p.second.calls ? (p.second.totalCycles / p.second.calls) : 0;
        uint64_t avgI = p.second.calls ? (p.second.totalInstrs / p.second.calls) : 0;
        std::printf("[probe] dc14 pc start=0x%08X calls=%u hit_cap=%u avg_cycles=%llu avg_instrs=%llu\n",
                    p.first, p.second.calls, p.second.hitCap, (unsigned long long)avgC, (unsigned long long)avgI);
    }

    static uint32_t s_lastMscalCount = 0;
    uint32_t curMscal = g_diagMscalCount.load(std::memory_order_relaxed);
    uint32_t deltaMscal = curMscal - s_lastMscalCount;
    s_lastMscalCount = curMscal;
    std::printf("[probe] dc14 vif1 mscal_calls=%u\n", deltaMscal);

    // Reset per-second stats
    s_dyCalls = 0;
    s_dyMscnt = 0;
    s_dyEndedEbit = 0;
    s_dyHitCap = 0;
    s_dyOther = 0;
    s_dyTotalInstrs = 0;
    s_dyTotalCycles = 0;
    s_dyMaxInstrs = 0;
    s_dyMaxCycles = 0;
    s_dyPcStats.clear();
    std::fflush(stdout);
}


namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

// PERF-GL: occupancy bitmasks for the pending-write pipelines so commitReadyPipelines walks only live slots.
// Indexed by unit (VU0/VU1 are separate instances); every site that sets/clears `.valid` maintains these.
struct PipeMasks
{
    uint32_t flag = 0, store = 0, vf = 0, vi = 0, acc = 0;
};
static PipeMasks s_pipeMasks[2];
static inline uint32_t pmCtz(uint32_t v)
{
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx = 0;
    _BitScanForward(&idx, v);
    return static_cast<uint32_t>(idx);
#else
    return static_cast<uint32_t>(__builtin_ctz(v));
#endif
}
#define PM() (s_pipeMasks[m_unit == Unit::VU1 ? 1 : 0])
// PERF-HM: earliest cycle at which any pending write can land; push sites reset it to 0 (rescan), commit recomputes it.
static bool s_vfyRef = false;                         // PERF-GR: true => use the original (reference) code paths, for A/B verification
static uint64_t s_nextReady[2] = {0u, 0u};
#define NR() (s_nextReady[m_unit == Unit::VU1 ? 1 : 0])

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    // PERF-HM: entries are zeroed when committed, so with nothing pending the arrays are already clear
    const PipeMasks &pmr = PM();
    if (s_vfyRef || (pmr.flag | pmr.store | pmr.vf | pmr.vi | pmr.acc) != 0u || m_fdiv.valid || m_efu[0].valid || m_efu[1].valid)
    {
        m_flagPipeline = {};
        m_fdiv = {};
        m_efu = {};
        m_storePipeline = {};
        m_vfWritePipeline = {};
        m_viWritePipeline = {};
        m_accWritePipeline = {};
    }
    PM() = {};
    NR() = 0u;
    // PERF-GH: do not zero the 64KB packet buffer (m_xgkick = {}) on every run; only control state matters
    m_xgkick.sourceAddress = 0; m_xgkick.totalBytes = 0; m_xgkick.copiedBytes = 0; m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0; m_xgkick.issueCycle = 0; m_xgkick.active = false; m_xgkick.currentTagEop = false;
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const auto q = [&]() { return operand(m_state.q); }; // PERF-HI lazy
    const auto i = [&]() { return operand(m_state.i); };

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q();
                break;
            case 0x1Eu:
                result = vs(component) * i();
                break;
            case 0x20u:
                result = vs(component) + q();
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q();
                break;
            case 0x22u:
                result = vs(component) + i();
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i();
                break;
            case 0x24u:
                result = vs(component) - q();
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q();
                break;
            case 0x26u:
                result = vs(component) - i();
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i();
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q();
            break;
        case 0x1Eu:
            result = vs(component) * i();
            break;
        case 0x20u:
            result = vs(component) + q();
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q();
            break;
        case 0x22u:
            result = vs(component) + i();
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i();
            break;
        case 0x24u:
            result = vs(component) - q();
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q();
            break;
        case 0x26u:
            result = vs(component) - i();
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i();
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

// PERF-GJ: fast mode (PS2X_VU1_FAST=1) drops the per-register hazard/latency scheduler: vf/vi/acc/flag/store
// writes land immediately; only Q/P (fdiv/efu) keep their latency. Cycle counts become ~1 per pair.
static std::vector<uint8_t> *s_vfyCapture = nullptr; // PROBE-GV: when set, XGKICK packets are captured instead of submitted
static int s_vfyFastOverride = -1;                    // PROBE-GV: -1 env, 0 force exact, 1 force fast
static inline int vu1VerifyMode() // PS2X_VU1_VERIFY: 1 = VU1 only, 2 = VU0 only, 3 = both
{
    static const int s_mode = []() { const char *v = std::getenv("PS2X_VU1_VERIFY"); return v ? std::atoi(v) : 0; }();
    return s_mode;
}
static const bool s_vu1FastEnv = []() { const char *v = std::getenv("PS2X_VU1_FAST"); return v && v[0] == '1'; }(); // PERF-HI
static inline bool vu1FastMode()
{
    if (s_vfyFastOverride >= 0)
        return s_vfyFastOverride == 1;
    return s_vu1FastEnv;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    if (vu1FastMode())
    {
        const uint32_t current = status & 0xFu;
        m_state.mac = mac;
        m_state.status = (m_state.status & 0xFF0u) | current | ((current | extraSticky) << 6);
        return;
    }

    FlagPipelineEntry *entry = nullptr;
    if (!s_vfyRef)
    {
        const uint32_t freeMask = ~PM().flag & ((1u << kMaxFlagEntries) - 1u);
        if (freeMask != 0u)
            entry = &m_flagPipeline[pmCtz(freeMask)];
    }
    else
    {
        for (FlagPipelineEntry &candidate : m_flagPipeline)
        {
            if (!candidate.valid)
            {
                entry = &candidate;
                break;
            }
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    PM().flag |= 1u << static_cast<uint32_t>(entry - m_flagPipeline.data()); NR() = 0u;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    if (vu1FastMode())
    {
        m_state.status = (m_state.status & 0x03Fu) | (static_cast<uint32_t>(immediate) & 0xFC0u);
        return;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            PM().flag |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data()); NR() = 0u;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    if (vu1FastMode())
    {
        m_state.clip = m_workingClip;
        return;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            PM().flag |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data()); NR() = 0u;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    if (vu1FastMode())
    {
        m_state.clip = m_workingClip;
        return;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            PM().flag |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data()); NR() = 0u;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    if (vu1FastMode()) // PERF-GJ2: stalls are not modelled in fast mode, so Q must land immediately (log gp: stale Q => texcoord diffs)
    {
        m_state.q = value;
        const uint32_t di = statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | di | (di << 6);
        return;
    }
    m_fdiv.valid = true; NR() = 0u;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    if (vu1FastMode()) // PERF-GJ2
    {
        m_state.p = value;
        return;
    }
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true; NR() = 0u;
            entry.readyCycle = m_cycle + latency;
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    if (s_egCurRunTraced && s_egTotalPairs < 700u)
    {
        uint32_t qw = (address / 16u) & 0x3FFu;
        if (qw >= 440u && qw <= 720u && s_ehStoreCount < 1024u)
        {
            const char *kind = "SQ";
            uint32_t op = (s_ehCurLowerInstr >> 25) & 0x7Fu;
            if (op == 0x01u) kind = "SQ";
            else if (op == 0x05u) kind = "ISW";
            else if (op == 0x00u) {
                uint32_t fn = s_ehCurLowerInstr & 0x3Fu;
                if (fn == 0x35u) kind = "SQI";
                else if (fn == 0x37u) kind = "SQD";
                else if (fn == 0x3Fu) kind = "ISWR";
            }
            uint32_t finalWords[4]{};
            if (m_activeVuData && (qw + 1u) * 16u <= m_activeVuDataSize) {
                std::memcpy(finalWords, m_activeVuData + qw * 16u, 16);
            }
            for (uint32_t c = 0; c < 4u; ++c) {
                if ((laneMask & laneForComponent(c)) != 0u) {
                    finalWords[c] = words[c];
                }
            }
            auto &rec = s_ehStores[s_ehStoreCount++];
            rec.pair = s_egTotalPairs;
            rec.pc = m_state.pc;
            rec.qw = qw;
            std::memcpy(rec.words, finalWords, sizeof(finalWords));
            rec.kind = kind;
        }
    }
    if (isVu3dProbeEnabled())
    {
        uint32_t qw = (address / 16u) & 0x3FFu;
        ps2_fe_recordVuStore(qw, m_state.pc);
        {
            // Task FF: run-length log of which program (pc) stores into the 636..698 / 716..778 tables, with run id
            static uint32_t s_ffLastKey = 0xFFFFFFFFu;
            static uint32_t s_ffRepeat = 0;
            static uint32_t s_ffLines = 0;
            {
                // Task FY: snapshot VU1 memory + registers at the 200th/600th store to qw 636 (table fill) for offline checking
                static uint32_t s_fyCount = 0;
                if (qw == 636u)
                {
                    ++s_fyCount;
                    if (s_fyCount == 200u || s_fyCount == 600u)
                    {
                        char path[64];
                        std::snprintf(path, sizeof(path), "logs/vu1_fill_%u.bin", s_fyCount);
                        if (FILE *ff = std::fopen(path, "wb"))
                        {
                            if (m_activeVuData)
                                std::fwrite(m_activeVuData, 1, m_activeVuDataSize < 16384u ? m_activeVuDataSize : 16384u, ff);
                            std::fclose(ff);
                        }
                        std::printf("[probe] fy fill snapshot #%u pc=0x%X vi=", s_fyCount, m_state.pc);
                        for (int r = 0; r < 16; ++r)
                            std::printf("%d ", m_state.vi[r]);
                        std::printf("\n");
                        for (int r = 0; r < 32; ++r)
                        {
                            const uint32_t *w = reinterpret_cast<const uint32_t *>(m_state.vf[r]);
                            std::printf("[probe] fy vf%02d=%08X %08X %08X %08X\n", r, w[0], w[1], w[2], w[3]);
                        }
                        std::fflush(stdout);
                    }
                }
            }
            const uint32_t region = (qw >= 636u && qw <= 698u) ? 1u : ((qw >= 716u && qw <= 778u) ? 2u : 0u);
            if (region != 0u)
            {
                const uint32_t key = (region << 24) | (m_state.pc & 0xFFFFu);
                static uint64_t s_ffLastRun = ~0ull;
                if (key != s_ffLastKey || s_elCurRunId != s_ffLastRun)
                {
                    if (s_ffRepeat > 0u && s_ffLines < 400u)
                    {
                        std::printf("[probe] ff wr_end x%u\n", s_ffRepeat);
                    }
                    s_ffLastKey = key;
                    s_ffLastRun = s_elCurRunId;
                    s_ffRepeat = 0;
                    if (s_ffLines++ < 400u)
                    {
                        const uint32_t *vf = reinterpret_cast<const uint32_t *>(m_state.vf[6]);
                        const uint32_t *vf5 = reinterpret_cast<const uint32_t *>(m_state.vf[5]);
                        std::printf("[probe] ff wr run=%llu region=%u pc=0x%X qw=%u vi1=%d vi5=%d vf6=%08X %08X %08X %08X vf5=%08X %08X %08X %08X\n",
                                    (unsigned long long)s_elCurRunId, region, m_state.pc, qw, m_state.vi[1], m_state.vi[5],
                                    vf[0], vf[1], vf[2], vf[3], vf5[0], vf5[1], vf5[2], vf5[3]);
                        std::fflush(stdout);
                    }
                }
                ++s_ffRepeat;
            }
        }
        if (qw >= 235u && qw <= 244u && ps2_vif1_getEuMatchedCount() >= 6u)
        {
            if (!s_euStoreLoggedThisRun)
            {
                s_euStoreLoggedThisRun = true;
                static std::atomic<uint32_t> s_euVuStorePrintCount{0};
                uint32_t stIdx = s_euVuStorePrintCount.fetch_add(1, std::memory_order_relaxed);
                if (stIdx < 20u)
                {
                    std::printf("[probe] eu vustore pc=0x%X qw=%u w=%08X %08X %08X %08X\n",
                                m_state.pc, qw, words[0], words[1], words[2], words[3]);
                    std::fflush(stdout);
                }
            }
        }
        uint32_t op = (s_ehCurLowerInstr >> 25) & 0x7Fu;
        uint32_t special = (s_ehCurLowerInstr & 3u) | ((s_ehCurLowerInstr >> 4) & 0x7Cu);
        bool isSq = (op == 0x01u) || ((op >= 0x40u || (s_ehCurLowerInstr & 0x80000000u)) && (special == 0x35u || special == 0x37u));
        bool isStore = isSq || (op == 0x05u) || ((op >= 0x40u || (s_ehCurLowerInstr & 0x80000000u)) && special == 0x3Fu);
        if (isStore && qw >= 970u && qw <= 1011u)
        {
            bool isBad = false;
            for (int l = 0; l < 4; ++l) {
                if (words[l] == 0x7FFFFFFFu) { isBad = true; break; }
                float f = 0.0f;
                std::memcpy(&f, &words[l], 4);
                if (std::isnan(f) || std::isinf(f) || std::fabs(f) > 1.0e15f) {
                    isBad = true;
                    break;
                }
            }
            if (isBad)
            {
                s_eoVuStoreBadCount++;
                if (s_eoVuStorePrintCount < 20u)
                {
                    s_eoVuStorePrintCount++;
                    std::string seqSoFar;
                    for (size_t i = 0; i < s_ekCurRunTargets.size(); ++i) {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%s%03X", (i > 0 ? "," : ""), s_ekCurRunTargets[i]);
                        seqSoFar += buf;
                    }
                    std::printf("[probe] eo vustore pc=0x%03X qw=%u w=%08X %08X %08X %08X run=%llu seq=<%s>\n",
                                m_state.pc, qw, words[0], words[1], words[2], words[3],
                                (unsigned long long)s_elCurRunId, seqSoFar.c_str());
                    std::fflush(stdout);
                }

                static std::atomic<uint32_t> s_epDumpCount{0};
                uint32_t epIdx = s_epDumpCount.fetch_add(1, std::memory_order_relaxed);
                if (epIdx < 3u)
                {
                    ps2x_probeDump();
                    uint32_t vi13Val = static_cast<uint32_t>(m_state.vi[13]);
                    uint32_t vi12Val = static_cast<uint32_t>(m_state.vi[12]);
                    uint32_t dWords[4][4]{};
                    for (int o = -2; o <= 1; ++o) {
                        uint32_t q = (vi13Val + o) & 0x3FFu;
                        if (m_activeVuData && (q + 1u) * 16u <= m_activeVuDataSize) {
                            std::memcpy(dWords[o + 2], m_activeVuData + q * 16u, 16);
                        }
                    }
                    std::printf("[probe] ep vu vi13=%u vi12=%u qw[vi13-2]=%08X %08X %08X %08X qw[vi13-1]=%08X %08X %08X %08X qw[vi13]=%08X %08X %08X %08X qw[vi13+1]=%08X %08X %08X %08X\n",
                                vi13Val, vi12Val,
                                dWords[0][0], dWords[0][1], dWords[0][2], dWords[0][3],
                                dWords[1][0], dWords[1][1], dWords[1][2], dWords[1][3],
                                dWords[2][0], dWords[2][1], dWords[2][2], dWords[2][3],
                                dWords[3][0], dWords[3][1], dWords[3][2], dWords[3][3]);
                    std::fflush(stdout);
                }
            }
        }
        if (isSq && qw >= 636u && qw < 700u)
        {
            auto &elW = s_elLastWriter[qw - 636u];
            elW.written = true;
            elW.runId = s_elCurRunId;
            elW.startPc = s_dyCurStartPc;
            elW.pairIdx = s_elCurRunPairs;
            elW.allZero = (words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == 0);

            s_elCurRunStCount++;
            if (qw < s_elCurRunStMinQw) s_elCurRunStMinQw = qw;
            if (qw > s_elCurRunStMaxQw) s_elCurRunStMaxQw = qw;
        }
        if (s_ekCurRunActive && isSq && qw >= 636u && qw < 700u)
        {
            s_ekCurRunSt636++;
            if (words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == 0)
            {
                s_ekCurRunSt636Zero++;
            }
        }
        if (isSq && qw >= 636u && qw < 700u)
        {
            auto &lw = s_eiLastWriter[qw - 636u];
            lw.written = true;
            lw.pc = m_state.pc;
            lw.run = s_eiTotalRuns;
            lw.pair = s_eiCurRunPairs;
            lw.startPc = s_eiCurStartPc;
            std::memcpy(lw.words, words, 16);

            if (s_eiCurRunActive)
            {
                s_eiCurRunSt636++;
                bool isGarbage = false;
                for (int lane = 0; lane < 4; ++lane)
                {
                    uint32_t w = words[lane];
                    if (w == 0x7F7FFFFFu || w == 0xFF7FFFFFu) {
                        isGarbage = true;
                        break;
                    }
                    float f = 0.0f;
                    std::memcpy(&f, &w, 4);
                    if (std::isnan(f) || std::isinf(f) || std::fabs(f) > 1.0e8f) {
                        isGarbage = true;
                        break;
                    }
                }
                if (isGarbage)
                {
                    s_eiCurRunGarb++;
                    if (!s_eiHitDone)
                    {
                        s_eiHitDone = true;
                        const auto now = std::chrono::steady_clock::now();
                        uint32_t procSec = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - s_vdStartTime).count());
                        dumpEiDiagnostics("hit", m_state.pc, qw, words, procSec, false, m_state, m_cycle);
                    }
                }
            }
        }
    }
    if (vu1FastMode())
    {
        if (m_activeVuData && address + 16u <= m_activeVuDataSize)
        {
            uint32_t cur[4];
            std::memcpy(cur, m_activeVuData + address, sizeof(cur));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    cur[component] = words[component];
            }
            std::memcpy(m_activeVuData + address, cur, sizeof(cur));
        }
        return;
    }
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            PM().store |= 1u << static_cast<uint32_t>(&store - m_storePipeline.data()); NR() = 0u;
            store.readyCycle = m_cycle + 1u;
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    PendingVfWrite *vfSlot = nullptr;
    if (!s_vfyRef)
    {
        const uint32_t freeMask = ~PM().vf & ((1u << kMaxPendingVfWrites) - 1u);
        if (freeMask != 0u)
            vfSlot = &m_vfWritePipeline[pmCtz(freeMask)];
    }
    else
    {
        for (PendingVfWrite &w : m_vfWritePipeline)
        {
            if (!w.valid)
            {
                vfSlot = &w;
                break;
            }
        }
    }
    if (vfSlot)
    {
        PendingVfWrite &write = *vfSlot;
        {
            write = {};
            write.valid = true;
            PM().vf |= 1u << static_cast<uint32_t>(&write - m_vfWritePipeline.data()); NR() = 0u;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            PM().vi |= 1u << static_cast<uint32_t>(&write - m_viWritePipeline.data()); NR() = 0u;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            PM().acc |= 1u << static_cast<uint32_t>(&write - m_accWritePipeline.data()); NR() = 0u;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    if (!vu1FastMode()) // PERF-HI: nothing pending -> nothing to commit
    {
        if (!s_vfyRef && m_cycle < NR()) // PERF-HM: nothing can land yet
            return;
        const PipeMasks &pm = PM();
        if ((pm.flag | pm.store | pm.vf | pm.vi | pm.acc) == 0u && !m_fdiv.valid && !m_efu[0].valid && !m_efu[1].valid)
        {
            NR() = ~0ull;
            return;
        }
    }
    if (vu1FastMode())
    {
        if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        for (ScalarPipelineEntry &entry : m_efu)
        {
            if (entry.valid && entry.readyCycle <= m_cycle)
            {
                m_state.p = entry.value;
                entry = {};
            }
        }
        return;
    }
    for (uint32_t mk = PM().flag; mk != 0u; mk &= mk - 1u)
    {
        const uint32_t slot = pmCtz(mk);
        FlagPipelineEntry &entry = m_flagPipeline[slot];
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;
        PM().flag &= ~(1u << slot);

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
    }

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
    }

    for (uint32_t mk = PM().store; mk != 0u; mk &= mk - 1u)
    {
        const uint32_t slot = pmCtz(mk);
        PendingStore &store = m_storePipeline[slot];
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
        PM().store &= ~(1u << slot);
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
    }

    for (uint32_t mk = PM().vf; mk != 0u; mk &= mk - 1u)
    {
        const uint32_t slot = pmCtz(mk);
        PendingVfWrite &write = m_vfWritePipeline[slot];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        PM().vf &= ~(1u << slot);
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
    }

    for (uint32_t mk = PM().vi; mk != 0u; mk &= mk - 1u)
    {
        const uint32_t slot = pmCtz(mk);
        PendingViWrite &write = m_viWritePipeline[slot];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        PM().vi &= ~(1u << slot);
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    for (uint32_t mk = PM().acc; mk != 0u; mk &= mk - 1u)
    {
        const uint32_t slot = pmCtz(mk);
        PendingAccWrite &write = m_accWritePipeline[slot];
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        PM().acc &= ~(1u << slot);
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
    }

    // PERF-HM: recompute the earliest pending ready cycle
    uint64_t nr = ~0ull;
    for (uint32_t mk = PM().flag; mk != 0u; mk &= mk - 1u) nr = std::min<uint64_t>(nr, m_flagPipeline[pmCtz(mk)].readyCycle);
    for (uint32_t mk = PM().store; mk != 0u; mk &= mk - 1u) nr = std::min<uint64_t>(nr, m_storePipeline[pmCtz(mk)].readyCycle);
    for (uint32_t mk = PM().vf; mk != 0u; mk &= mk - 1u) nr = std::min<uint64_t>(nr, m_vfWritePipeline[pmCtz(mk)].readyCycle);
    for (uint32_t mk = PM().vi; mk != 0u; mk &= mk - 1u) nr = std::min<uint64_t>(nr, m_viWritePipeline[pmCtz(mk)].readyCycle);
    for (uint32_t mk = PM().acc; mk != 0u; mk &= mk - 1u) nr = std::min<uint64_t>(nr, m_accWritePipeline[pmCtz(mk)].readyCycle);
    if (m_fdiv.valid) nr = std::min<uint64_t>(nr, m_fdiv.readyCycle);
    for (const ScalarPipelineEntry &entry : m_efu) if (entry.valid) nr = std::min<uint64_t>(nr, entry.readyCycle);
    NR() = nr;
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
            m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

thread_local uint64_t s_currentRunXgkickNs = 0;

static uint32_t countPrimsInXgkickPacket(const uint8_t *data, uint32_t size)
{
    uint32_t totalPrims = 0;
    uint32_t offset = 0;
    while (offset + 16u <= size)
    {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, data + offset, sizeof(tagLo));
        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u) nreg = 16u;
        bool pre = ((tagLo >> 46) & 1u) != 0u;
        uint32_t primType = static_cast<uint32_t>((tagLo >> 47) & 0x7u);

        uint32_t tagBytes = 16u;
        if (format == 0u)
        {
            tagBytes += nloop * nreg * 16u;
            if (pre)
            {
                if (primType == 6u) // SPRITE
                    totalPrims += (nloop / 2u);
                else if (primType == 3u || primType == 4u || primType == 5u)
                    totalPrims += (nloop >= 3 ? nloop - 2 : 0);
                else
                    totalPrims += nloop;
            }
            else
            {
                totalPrims += nloop;
            }
        }
        else if (format == 1u)
        {
            tagBytes += ((nloop * nreg + 1u) & ~1u) * 8u;
            totalPrims += nloop;
        }
        else if (format == 2u)
        {
            tagBytes += nloop * 16u;
        }
        else
        {
            break;
        }
        if (tagBytes < 16u || offset + tagBytes > size)
            break;
        offset += tagBytes;
        if (((tagLo >> 15) & 1u) != 0u) // EOP
            break;
    }
    return totalPrims;
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (probe_ae::isEnabled() || probe_ae::isMovieHleVerify())
    {
        probe_ae::onBbXgkickPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
        probe_ae::onBxOrigRunXgkick(m_xgkick.packet.data(), m_xgkick.totalBytes);
    }

    const auto tXgkickStart = std::chrono::steady_clock::now();
    if (s_vfyCapture)
        s_vfyCapture->insert(s_vfyCapture->end(), m_xgkick.packet.data(), m_xgkick.packet.data() + m_xgkick.totalBytes);
    else if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    const auto tXgkickEnd = std::chrono::steady_clock::now();

    if (probe_ae::isEnabled())
    {
        uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tXgkickEnd - tXgkickStart).count());
        s_currentRunXgkickNs += ns;
        uint32_t prims = countPrimsInXgkickPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
        probe_ae::onBaXgkick(m_xgkick.totalBytes / 16u, ns, prims);
    }
    if (isVu3dProbeEnabled())
    {
        s_vdCurRunKicks++;
        decodeXgkickPacketStats(m_xgkick.packet.data(), m_xgkick.totalBytes,
                                s_vdCurRunVerts, s_vdCurRunZeroXy, s_vdCurRunOob,
                                s_vdCurRunPrimHist);
        if (s_egCurRunTraced && s_egCurRunPackets.size() < 2u)
        {
            EePacket pkt{};
            uint32_t qwAvailable = m_xgkick.totalBytes / 16u;
            pkt.qwCount = std::min<uint32_t>(24u, qwAvailable);
            std::memset(pkt.words, 0, sizeof(pkt.words));
            std::memcpy(pkt.words, m_xgkick.packet.data(), pkt.qwCount * 16u);
            s_egCurRunPackets.push_back(pkt);
        }
        if (s_emCurRunCandidate && s_emCurRunPackets.size() < 3u)
        {
            EePacket pkt{};
            uint32_t qwAvailable = m_xgkick.totalBytes / 16u;
            pkt.qwCount = std::min<uint32_t>(64u, qwAvailable);
            std::memset(pkt.words, 0, sizeof(pkt.words));
            std::memcpy(pkt.words, m_xgkick.packet.data(), pkt.qwCount * 16u);
            s_emCurRunPackets.push_back(pkt);
        }
        if (!s_eeVuDone && s_eeCurRunPackets.size() < 4u)
        {
            EePacket pkt{};
            uint32_t qwAvailable = m_xgkick.totalBytes / 16u;
            pkt.qwCount = std::min<uint32_t>(64u, qwAvailable);
            std::memset(pkt.words, 0, sizeof(pkt.words));
            std::memcpy(pkt.words, m_xgkick.packet.data(), pkt.qwCount * 16u);
            s_eeCurRunPackets.push_back(pkt);
        }
        if (!s_vdHaveFirstXgkick)
        {
            s_vdHaveFirstXgkick = true;
            uint32_t copyCount = std::min<uint32_t>(14u, m_xgkick.totalBytes / 16u);
            std::memset(s_vdFirstXgkickWords, 0, sizeof(s_vdFirstXgkickWords));
            std::memcpy(s_vdFirstXgkickWords, m_xgkick.packet.data(), copyCount * 16u);
        }
    }
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    // PERF-GH: do not zero the 64KB packet buffer (m_xgkick = {}) on every run; only control state matters
    m_xgkick.sourceAddress = 0; m_xgkick.totalBytes = 0; m_xgkick.copiedBytes = 0; m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0; m_xgkick.issueCycle = 0; m_xgkick.active = false; m_xgkick.currentTagEop = false;
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    if (s_eiCurRunActive)
    {
        s_eiCurRunActive = false;
        uint32_t hIdx = s_eiRunHistHead % 12u;
        s_eiRunHist[hIdx] = {s_eiTotalRuns, s_eiCurStartPc, (m_cycle > s_eiRunStartCycle ? (m_cycle - s_eiRunStartCycle) : 0u),
                             s_eiCurRunSt636, s_eiCurRunGarb, s_eiCurRunDisp};
        s_eiRunHistHead = (s_eiRunHistHead + 1u) % 12u;
        if (s_eiRunHistCount < 12u) s_eiRunHistCount++;
    }
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        if (s_vfyRef)
        {
            for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
            {
                if ((usage->viRead & (1u << reg)) != 0u)
                    ready = std::max(ready, m_viReady[reg]);
            }
        }
        else
        {
            for (uint32_t mk = static_cast<uint32_t>(usage->viRead) & 0xFFFEu; mk != 0u; mk &= mk - 1u)
                ready = std::max(ready, m_viReady[pmCtz(mk)]);
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    if (s_vfyRef)
    {
        for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
        {
            if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
                m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
        }
    }
    else
    {
        const uint64_t viReadyAt = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
        for (uint32_t mk = static_cast<uint32_t>(decoded.lowerUsage.viWrite) & 0xFFFEu; mk != 0u; mk &= mk - 1u)
            m_viReady[pmCtz(mk)] = viReadyAt;
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

static uint64_t s_hcCurHash = 0; // PROBE-HC: hash of the VU1 microcode image currently loaded
void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;

    { // PROBE-HC: dump every distinct microcode image once (input for the VU1 native-region generator)
        static std::set<uint64_t> s_hcSeen;
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < codeSize; ++i)
            h = (h ^ vuCode[i]) * 1099511628211ull;
        if (m_unit == Unit::VU1)
            s_hcCurHash = h;
        bool hcNew = false;
        {
            static std::mutex s_hcMutex; // PERF-HN: VU0 (EE thread) and VU1 (VIF1 worker) both rebuild decode caches
            std::lock_guard<std::mutex> hcLock(s_hcMutex);
            hcNew = s_hcSeen.insert(h ^ (m_unit == Unit::VU1 ? 0x1ull : 0x2ull)).second && s_hcSeen.size() <= 64u;
        }
        if (hcNew)
        {
            char path[128];
            std::snprintf(path, sizeof(path), "logs/vucode_u%d_%016llx.bin", m_unit == Unit::VU1 ? 1 : 0, static_cast<unsigned long long>(h));
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(vuCode, 1, codeSize, f);
                std::fclose(f);
            }
            // per-pair usage tables exactly as the scheduler sees them (input for the generator; avoids porting the decode tables)
            std::snprintf(path, sizeof(path), "logs/vuusage_u%d_%016llx.txt", m_unit == Unit::VU1 ? 1 : 0, static_cast<unsigned long long>(h));
            if (FILE *uf = std::fopen(path, "w"))
            {
                auto dumpUsage = [&](const char *tag, const InstructionUsage &u) {
                    std::fprintf(uf, " %s r=%u:%u:%u:%u:%u w=%u:%u vir=%x viw=%x ar=%u aw=%u lat=%u vfl=%u vil=%u pipe=%u wq=%d wp=%d rc=%d wc=%d dnb=%d rsv=%d", tag,
                                 u.vfReadCount, u.vfRead[0].reg, u.vfRead[0].lanes, u.vfRead[1].reg, u.vfRead[1].lanes,
                                 u.vfWrite.reg, u.vfWrite.lanes, static_cast<unsigned>(u.viRead), static_cast<unsigned>(u.viWrite),
                                 u.accRead, u.accWrite, u.latency, u.vfLatency, u.viLatency, static_cast<unsigned>(u.pipeline),
                                 u.waitQ ? 1 : 0, u.waitP ? 1 : 0, u.readsClip ? 1 : 0, u.writesClip ? 1 : 0, u.delaysNextBranchRead ? 1 : 0, u.reserved ? 1 : 0);
                };
                for (uint32_t i = 0; i < pairCount; ++i)
                {
                    const DecodedInstructionPair &dp = m_decodedCodeCache[i];
                    std::fprintf(uf, "P %04X %08X %08X i=%d e=%d m=%d d=%d t=%d shadow=%u supp=%u", i * 8u, dp.upper, dp.lower,
                                 dp.iBit ? 1 : 0, dp.eBit ? 1 : 0, dp.mBit ? 1 : 0, dp.dBit ? 1 : 0, dp.tBit ? 1 : 0,
                                 dp.upperVfShadowReg, dp.suppressedLowerVf);
                    dumpUsage("L", dp.lowerUsage);
                    dumpUsage("U", dp.upperUsage);
                    std::fprintf(uf, "\n");
                }
                std::fclose(uf);
            }
            std::printf("[probe] hc vucode unit=%d hash=%016llx size=%u gen=%llu\n", m_unit == Unit::VU1 ? 1 : 0,
                        static_cast<unsigned long long>(h), codeSize, static_cast<unsigned long long>(generation));
            std::fflush(stdout);
        }
    }
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    s_dyCurIsMscnt = false;
    s_dyCurStartPc = startPC;
    g_diagMscalCount.fetch_add(1, std::memory_order_relaxed);
    s_dyCurTop = top;
    s_dyCurItop = itop;
    s_dzPendingRun = false;
    s_vdCurRunKicks = 0;
    s_vdCurRunVerts = 0;
    s_vdCurRunZeroXy = 0;
    s_vdCurRunOob = 0;
    std::memset(s_vdCurRunPrimHist, 0, sizeof(s_vdCurRunPrimHist));
    s_vdHaveFirstXgkick = false;
    s_eeCurRunPackets.clear();
    s_egCurRunTraced = false;
    s_egCurRunPackets.clear();
    s_egTotalPairs = 0;
    s_emCurRunCandidate = false;
    s_emRecording = false;
    if (isVu3dProbeEnabled() && !s_emDone && (startPC & 0x7FFFFFFF) == 0x08u)
    {
        s_emCurRunCandidate = true;
        s_emRecording = true;
        s_emCurRunPairs = 0;
        s_emTraceCount = 0;
        s_emLastPairPc = 0xFFFFFFFFu;
        s_emCurRunTargets.clear();
        s_emCurRunPackets.clear();
        s_emLowOpHist.clear();
        std::memcpy(s_emEntryVf, m_state.vf, sizeof(s_emEntryVf));
        std::memcpy(s_emEntryVi, m_state.vi, sizeof(s_emEntryVi));
        s_emEntryTop = top;
        s_emEntryItop = itop;
        s_emEntryQ = m_state.q;
        s_emEntryP = m_state.p;
        if (vuData && dataSize >= 16384u) {
            std::memcpy(s_emEntryVuData, vuData, 16384);
        }
    }
    s_elCurRunId = ++s_elGlobalRunId;
    s_elCurRunPairs = 0;
    s_elCurRunStCount = 0;
    s_elCurRunStMinQw = 9999;
    s_elCurRunStMaxQw = 0;
    s_elCurRunRdCount = 0;
    s_elCurRunRdMinQw = 9999;
    s_elCurRunRdMaxQw = 0;
    s_elCurRunRdZero = 0;
    s_elCurRunRdNever = 0;
    s_elCurRunRdMinAge = 0xFFFFFFFFFFFFFFFFULL;
    s_elCurRunRdMaxAge = 0;
    s_elCurRunHaveAge = false;
    s_ekCurRunActive = false;
    if (isVu3dProbeEnabled() && (startPC & 0x7FFFFFFF) == 0x08u)
    {
        s_ekCurRunActive = true;
        s_ekCurRunPairs = 0;
        s_ekCurRunSt636 = 0;
        s_ekCurRunSt636Zero = 0;
        s_ekLastPairPc = 0xFFFFFFFFu;
        s_ekCurRunTargets.clear();
        if (vuData && dataSize >= 16384u) {
            std::memcpy(s_ekEntryVuData, vuData, 16384);
        }
    }
    s_eiCurRunActive = false;
    if (isVu3dProbeEnabled() && (startPC & 0x7FFFFFFF) == 0x08u && (!s_eiHitDone || !s_eiReadDone))
    {
        const auto eiNow = std::chrono::steady_clock::now();
        const uint64_t procSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(eiNow - s_vdStartTime).count());
        if (procSec >= getVu3dAfterSec())
        {
            s_eiCurRunActive = true;
            s_eiTotalRuns++;
            s_eiCurRunPairs = 0;
            s_eiCurRunSt636 = 0;
            s_eiCurRunGarb = 0;
            s_eiLastPairPc = 0xFFFFFFFFu;
            s_eiCurRunDisp.clear();
            s_eiCurStartPc = (startPC & 0x7FFFFFFFu);
            s_eiCurTop = top;
            s_eiCurItop = itop;
            s_eiRunStartCycle = m_cycle;
            s_eiRingHead = 0;
            s_eiRingCount = 0;
            if (vuData && dataSize >= 16384u) {
                std::memcpy(s_eiEntryVuData, vuData, 16384);
            }
        }
    }
    if (!s_egDone && isVu3dProbeEnabled() && (startPC & 0x7FFFFFFF) == 0x08u)
    {
        const auto ecNow = std::chrono::steady_clock::now();
        const uint64_t procSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(ecNow - s_vdStartTime).count());
        if (procSec >= 60u)
        {
            uint32_t rIdx = s_egPc08RunCount++;
            if ((rIdx % 20u) == 0u)
            {
                s_egCurRunTraced = true;
                s_egTraceCount = 0;
                s_ehStoreCount = 0;
                std::memcpy(s_egEntryVf, m_state.vf, sizeof(s_egEntryVf));
                std::memcpy(s_egEntryVi, m_state.vi, sizeof(s_egEntryVi));
                s_egEntryTop = top;
                s_egEntryItop = itop;
                for (uint32_t q = 440u; q <= 720u; ++q) {
                    if (vuData && (q + 1u) * 16u <= dataSize) {
                        std::memcpy(s_egEntryDataWords[q - 440u], vuData + q * 16u, 16);
                    } else {
                        std::memset(s_egEntryDataWords[q - 440u], 0, 16);
                    }
                }
            }
        }
    }
    if (probe_ae::isDcProbeEnabled() && (startPC & 0x7FFFFFFF) == 0x08u) {
        uint64_t vsyncTick = 0;
        uint8_t *ram = memory ? memory->getRDRAM() : nullptr;
        if (ram) {
            std::memcpy(&vsyncTick, ram + (0x696FD0u & 0x1FFFFFFFu), 8);
        }
        if (vsyncTick > 600u) {
            s_dzPendingRun = true;
            s_dzPendingStartPc = startPC;
            s_dzPendingTop = top;
            s_dzPendingItop = itop;
            std::memcpy(s_dzPendingVi, m_state.vi, sizeof(s_dzPendingVi));
            int32_t baseQw = static_cast<int32_t>(top) - 2;
            for (int i = 0; i < 32; ++i) {
                int32_t q = baseQw + i;
                uint32_t effQw = (q >= 0) ? (static_cast<uint32_t>(q) & 0x3FFu) : ((static_cast<uint32_t>(q + 1024)) & 0x3FFu);
                if (vuData && (effQw + 1u) * 16u <= dataSize) {
                    std::memcpy(s_dzPendingDataWords[i], vuData + effQw * 16u, 16);
                } else {
                    std::memset(s_dzPendingDataWords[i], 0, 16);
                }
            }
        }
    }

    if (probe_ae::handleMovieHleMscal(startPC, vuCode, codeSize, vuData, dataSize, top, gs, memory))
    {
        return;
    }

    probe_ae::flushMovieBatch(&gs, memory);

    probe_ae::onBxOrigRunStart(startPC, top, vuData, dataSize);
    probe_ae::onBzOrigRunBlock();

    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    if (probe_ae::isEnabled() || probe_ae::isMovieHleVerify())
    {
        probe_ae::onBaMscalStart(startPC, m_state.vi);
        probe_ae::onBbRunStart(startPC, vuCode, codeSize, vuData, dataSize,
                               m_state.vf, m_state.vi, m_state.acc,
                               m_state.q, m_state.p, m_state.i);
    }
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    if ((vu1VerifyMode() & (m_unit == Unit::VU1 ? 1 : 2)) != 0) // PROBE-GV: A/B check exact vs fast on identical input; game keeps the exact result
    {
        const VU1State s0 = m_state;
        const uint32_t wc0 = m_workingClip;
        const uint64_t cyc0 = m_cycle;
        std::vector<uint8_t> mem0(vuData, vuData + dataSize);
        std::vector<uint8_t> capA, capB;

        s_vfyFastOverride = 0;
        s_vfyRef = false;
        s_vfyCapture = &capA;
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        const VU1State sA = m_state;
        const uint32_t wcA = m_workingClip;
        const uint64_t cycA = m_cycle;
        std::vector<uint8_t> memA(vuData, vuData + dataSize);

        m_state = s0;
        m_workingClip = wc0;
        m_cycle = cyc0;
        std::memcpy(vuData, mem0.data(), dataSize);
        resetScheduler();
        s_vfyFastOverride = 0;
        s_vfyRef = true;
        s_vfyCapture = &capB;
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        const VU1State sB = m_state;

        int badVf = -1, badVi = -1;
        for (int r = 0; r < 32 && badVf < 0; ++r)
            if (std::memcmp(sA.vf[r], sB.vf[r], 16) != 0) badVf = r;
        for (int r = 0; r < 16 && badVi < 0; ++r)
            if (sA.vi[r] != sB.vi[r]) badVi = r;
        const bool badAcc = std::memcmp(sA.acc, sB.acc, 16) != 0;
        const bool badQpi = std::memcmp(&sA.q, &sB.q, 4) != 0 || std::memcmp(&sA.p, &sB.p, 4) != 0 || std::memcmp(&sA.i, &sB.i, 4) != 0;
        const bool badFlags = sA.mac != sB.mac || sA.status != sB.status || sA.clip != sB.clip;
        int badMemQw = -1;
        for (uint32_t q = 0; q * 16u + 16u <= dataSize && badMemQw < 0; ++q)
            if (std::memcmp(memA.data() + q * 16u, vuData + q * 16u, 16) != 0) badMemQw = static_cast<int>(q);
        const bool badGif = capA != capB;

        static uint32_t s_gvTotalU[2] = {0, 0}, s_gvBadU[2] = {0, 0}, s_gvPrintedU[2] = {0, 0};
        const int gvU = (m_unit == Unit::VU1) ? 1 : 0;
        uint32_t &s_gvTotal = s_gvTotalU[gvU], &s_gvBad = s_gvBadU[gvU], &s_gvPrinted = s_gvPrintedU[gvU];
        ++s_gvTotal;
        const bool bad = badVf >= 0 || badVi >= 0 || badAcc || badQpi || badFlags || badMemQw >= 0 || badGif;
        if (bad)
            ++s_gvBad;
        if (bad && s_gvPrinted < 40u)
        {
            ++s_gvPrinted;
            std::printf("[probe] gv MISMATCH u%d #%u start=0x%X vf=%d vi=%d acc=%d qpi=%d flags=%d(mac %X/%X st %X/%X clip %X/%X) memqw=%d gif=%d(%zu/%zu)\n",
                        gvU, s_gvPrinted, startPC & 0xFFFFu, badVf, badVi, badAcc ? 1 : 0, badQpi ? 1 : 0, badFlags ? 1 : 0,
                        sA.mac, sB.mac, sA.status, sB.status, sA.clip, sB.clip, badMemQw, badGif ? 1 : 0, capA.size(), capB.size());
            if (badVf >= 0)
            {
                uint32_t a4[4], b4[4];
                std::memcpy(a4, sA.vf[badVf], 16);
                std::memcpy(b4, sB.vf[badVf], 16);
                std::printf("[probe] gv  vf%d exact=%08X %08X %08X %08X fast=%08X %08X %08X %08X\n", badVf,
                            a4[0], a4[1], a4[2], a4[3], b4[0], b4[1], b4[2], b4[3]);
            }
            if (badVi >= 0)
                std::printf("[probe] gv  vi%d exact=%d fast=%d\n", badVi, sA.vi[badVi], sB.vi[badVi]);
            if (badMemQw >= 0)
            {
                uint32_t a4[4], b4[4];
                std::memcpy(a4, memA.data() + badMemQw * 16, 16);
                std::memcpy(b4, vuData + badMemQw * 16, 16);
                std::printf("[probe] gv  mem qw%d exact=%08X %08X %08X %08X fast=%08X %08X %08X %08X\n", badMemQw,
                            a4[0], a4[1], a4[2], a4[3], b4[0], b4[1], b4[2], b4[3]);
            }
            if (badGif)
            {
                size_t off = 0;
                const size_t n = std::min(capA.size(), capB.size());
                while (off < n && capA[off] == capB[off])
                    ++off;
                std::printf("[probe] gv  gif first diff at byte %zu\n", off);
            }
            std::fflush(stdout);
        }
        if ((s_gvTotal % 500u) == 0u)
        {
            std::printf("[probe] gv summary u%d runs=%u mismatches=%u\n", gvU, s_gvTotal, s_gvBad);
            std::fflush(stdout);
        }

        // keep the exact result for the game
        m_state = sA;
        m_workingClip = wcA;
        m_cycle = cycA;
        std::memcpy(vuData, memA.data(), dataSize);
        s_vfyFastOverride = -1;
        s_vfyRef = false;
        s_vfyCapture = nullptr;
        if (!capA.empty() && memory)
            memory->submitGifPacket(GifPathId::Path1, capA.data(), static_cast<uint32_t>(capA.size()));
        return;
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    if (probe_ae::isEnabled())
    {
        probe_ae::onBaMscntStart(m_state.vi);
        probe_ae::onBbRunStart(m_state.pc, vuCode, codeSize, vuData, dataSize,
                               m_state.vf, m_state.vi, m_state.acc,
                               m_state.q, m_state.p, m_state.i);
    }
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

// =========================================================================
// Task FC: VU1 C80 10-qw matrix block audit
// =========================================================================
extern "C" bool ps2_vif1_findLastUnpackForQw(uint32_t qw, uint32_t *outDest, uint32_t *outNum, uint32_t *outSrc);

struct FcC80State {
    bool active = false;
    uint32_t vi13 = 0;
    uint32_t dst = 0;
    uint32_t copied = 0;
    uint32_t top = 0;
    uint32_t mscal_pc = 0;
};

static thread_local FcC80State s_fcC80;
static std::atomic<uint32_t> s_fcTotal{0};
static std::atomic<uint32_t> s_fcFlaggedNonfinite{0};
static std::atomic<uint32_t> s_fcFlaggedBig{0};
static std::atomic<uint32_t> s_fcZeroRows{0};
static std::atomic<uint32_t> s_fcFlaggedPrinted{0};
static std::atomic<uint32_t> s_fcUnflaggedPrinted{0};
static std::chrono::steady_clock::time_point s_fcLastSummaryTime{};

static void processFcC80Completion(const uint8_t *vuData, uint32_t dataSize)
{
    if (!s_fcC80.active || !vuData)
    {
        s_fcC80.active = false;
        return;
    }
    s_fcC80.active = false;

    uint32_t evNum = s_fcTotal.fetch_add(1, std::memory_order_relaxed) + 1u;
    uint32_t n = std::min(s_fcC80.copied, 10u);

    uint32_t block[10][4]{};
    bool eventNonfinite = false;
    bool eventBig = false;
    bool eventZeroRow = false;

    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t q = (s_fcC80.dst + i) & 0x3FFu;
        if ((q + 1u) * 16u <= dataSize)
        {
            std::memcpy(block[i], vuData + q * 16u, 16);
        }

        // Check zero row: all 4 words 0 in qw index 0..3
        if (i < 4u && block[i][0] == 0u && block[i][1] == 0u && block[i][2] == 0u && block[i][3] == 0u)
        {
            eventZeroRow = true;
        }

        for (int l = 0; l < 4; ++l)
        {
            uint32_t w = block[i][l];
            // Nonfinite: exp == 0xFF
            if (((w >> 23) & 0xFFu) == 0xFFu)
            {
                eventNonfinite = true;
            }
            else
            {
                float f = 0.0f;
                std::memcpy(&f, &w, 4);
                if (std::fabs(f) > 1e7f)
                {
                    eventBig = true;
                }
            }
        }
    }

    if (eventNonfinite) s_fcFlaggedNonfinite.fetch_add(1, std::memory_order_relaxed);
    if (eventBig) s_fcFlaggedBig.fetch_add(1, std::memory_order_relaxed);
    if (eventZeroRow) s_fcZeroRows.fetch_add(1, std::memory_order_relaxed);

    bool isFlagged = (eventNonfinite || eventBig || eventZeroRow);

    bool shouldPrint = false;
    if (isFlagged)
    {
        if (s_fcFlaggedPrinted.fetch_add(1, std::memory_order_relaxed) < 20u)
        {
            shouldPrint = true;
        }
    }
    else
    {
        if (s_fcUnflaggedPrinted.fetch_add(1, std::memory_order_relaxed) < 6u)
        {
            shouldPrint = true;
        }
    }

    if (shouldPrint)
    {
        std::string flagStr;
        if (eventNonfinite) flagStr += (flagStr.empty() ? "NF" : "|NF");
        if (eventBig) flagStr += (flagStr.empty() ? "BIG" : "|BIG");
        if (eventZeroRow) flagStr += (flagStr.empty() ? "ZR" : "|ZR");
        if (flagStr.empty()) flagStr = "OK";

        char srcStr[32];
        uint32_t uDest = 0, uNum = 0, uSrc = 0;
        bool hasSrc = ps2_vif1_findLastUnpackForQw(s_fcC80.vi13, &uDest, &uNum, &uSrc);
        if (!hasSrc)
        {
            hasSrc = ps2_vif1_findLastUnpackForQw(s_fcC80.dst, &uDest, &uNum, &uSrc);
        }
        if (hasSrc && uSrc != 0u)
        {
            std::snprintf(srcStr, sizeof(srcStr), "src=0x%08X", uSrc);
        }
        else
        {
            std::snprintf(srcStr, sizeof(srcStr), "src=?");
        }

        std::string qwStr;
        for (uint32_t q = 0; q < 10u; ++q)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " qw%u=%08X %08X %08X %08X",
                          q, block[q][0], block[q][1], block[q][2], block[q][3]);
            qwStr += buf;
        }

        std::printf("[probe] fc blk ev=%u flag=%s vi13=%u dst=%u top=%u mscal_pc=0x%03X %s%s\n",
                    evNum, flagStr.c_str(), s_fcC80.vi13, s_fcC80.dst, s_fcC80.top,
                    s_fcC80.mscal_pc, srcStr, qwStr.c_str());
        std::fflush(stdout);
    }
}

static uint64_t *s_geHistPtr = nullptr; // PROBE-GE2
void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;
    s_euStoreLoggedThisRun = false;

    s_currentRunXgkickNs = 0;
    const auto tRunStart = std::chrono::steady_clock::now();
    const uint64_t tRunStartCycle = m_cycle;
    uint32_t instrsThisRun = 0;

    DecodedInstructionPair decodedFallback;
    bool decodeCacheOk = false;
    if (memory != nullptr &&
        ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) || (m_unit == Unit::VU0 && vuCode == memory->getVU0Code())))
    {
        (void)getDecodedInstructionPairForPc(vuCode, codeSize, memory, 0u); // validate / rebuild the cache once per run
        const uint64_t gen = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
        decodeCacheOk = m_decodedCodeCacheValid && m_cachedVuCode == vuCode && m_cachedMemory == memory &&
                        m_cachedCodeSize == codeSize && m_cachedCodeGeneration == gen;
    }

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    // VU1 native executor (stage 1): generated per-pair functions (generated/vu1_native_*.cpp, tools/vu1gen).
    // Only for the known main image, VU1, exact scheduler mode, no probes, and never in the verifier's reference pass.
    // PERF-HL: specialised upper-pipe path for add/sub/mul/madd/msub families (vd or ACC target) and NOP.
    // Same maths as execUpper + normalizeFmacResult + calculateFmacProductSticky, without the per-lane decode.
    static const auto fastUpper = [](VU1Interpreter &v, uint32_t instr) -> bool
    {
        const uint32_t op = instr & 0x3Fu;
        uint32_t e = op;
        bool toAcc = false;
        if (op >= 0x3Cu)
        {
            e = (instr & 3u) | ((instr >> 4) & 0x7Cu);
            toAcc = true;
            if (e == 0x2Fu || e == 0x30u)
            {
                v.m_currentUpperInstruction = instr;
                return true;
            }
        }
        // kind: 0 add, 1 sub, 2 madd, 3 msub, 4 mul. src: 0 vt[bc], 1 q, 2 i, 3 vt[c]
        int kind, src, bc = 0;
        if (e <= 0x0Fu) { kind = static_cast<int>(e >> 2); src = 0; bc = static_cast<int>(e & 3u); }
        else if (e >= 0x18u && e <= 0x1Bu) { kind = 4; src = 0; bc = static_cast<int>(e & 3u); }
        else
        {
            switch (e)
            {
            case 0x1Cu: kind = 4; src = 1; break;
            case 0x1Eu: kind = 4; src = 2; break;
            case 0x20u: kind = 0; src = 1; break;
            case 0x21u: kind = 2; src = 1; break;
            case 0x22u: kind = 0; src = 2; break;
            case 0x23u: kind = 2; src = 2; break;
            case 0x24u: kind = 1; src = 1; break;
            case 0x25u: kind = 3; src = 1; break;
            case 0x26u: kind = 1; src = 2; break;
            case 0x27u: kind = 3; src = 2; break;
            case 0x28u: kind = 0; src = 3; break;
            case 0x29u: kind = 2; src = 3; break;
            case 0x2Au: kind = 4; src = 3; break;
            case 0x2Cu: kind = 1; src = 3; break;
            case 0x2Du: kind = 3; src = 3; break;
            default: return false;
            }
        }
        v.m_currentUpperInstruction = instr;
        const uint8_t dest = static_cast<uint8_t>((instr >> 21) & 0xFu);
        const uint32_t ft = (instr >> 16) & 0x1Fu;
        const uint32_t fs = (instr >> 11) & 0x1Fu;
        const uint32_t fd = (instr >> 6) & 0x1Fu;
        const bool madd = kind == 2 || kind == 3;
        float bval = 0.0f;
        if (src == 0) bval = v.normalizeOperand(v.m_state.vf[ft][bc]);
        else if (src == 1) bval = v.normalizeOperand(v.m_state.q);
        else if (src == 2) bval = v.normalizeOperand(v.m_state.i);

        const auto flagsOf = [](float &value, long double ex) -> uint8_t
        {
            const bool negative = std::signbit(ex);
            const long double magnitude = std::fabs(ex);
            uint8_t flags = negative ? 0x2u : 0u;
            uint32_t bits = negative ? 0x80000000u : 0u;
            if (magnitude == 0.0L)
            {
                flags |= 0x1u;
                std::memcpy(&value, &bits, sizeof(value));
            }
            else if (magnitude > static_cast<long double>(std::numeric_limits<float>::max()))
            {
                flags |= 0x8u;
                bits |= 0x7F7FFFFFu;
                std::memcpy(&value, &bits, sizeof(value));
            }
            else if (magnitude < static_cast<long double>(std::numeric_limits<float>::min()))
            {
                flags |= 0x5u;
                std::memcpy(&value, &bits, sizeof(value));
            }
            return flags;
        };

        float result[4];
        uint8_t laneFlags[4] = {0u, 0u, 0u, 0u};
        uint32_t extraSticky = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if ((dest & laneForComponent(c)) == 0u)
                continue;
            const float a = v.normalizeOperand(v.m_state.vf[fs][c]);
            const float b = src == 3 ? v.normalizeOperand(v.m_state.vf[ft][c]) : bval;
            const float ac = madd ? v.normalizeOperand(v.m_state.acc[c]) : 0.0f;
            float val;
            long double ex;
            switch (kind)
            {
            case 0: val = a + b; ex = static_cast<long double>(a) + static_cast<long double>(b); break;
            case 1: val = a - b; ex = static_cast<long double>(a) - static_cast<long double>(b); break;
            case 2: val = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac))); ex = static_cast<long double>(ac) + static_cast<long double>(a) * static_cast<long double>(b); break;
            case 3: val = _mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac))); ex = static_cast<long double>(ac) - static_cast<long double>(a) * static_cast<long double>(b); break;
            default: val = a * b; ex = static_cast<long double>(a) * static_cast<long double>(b); break;
            }
            laneFlags[c] = flagsOf(val, ex);
            result[c] = val;
            if (madd)
            {
                float pv = a * b;
                extraSticky |= flagsOf(pv, static_cast<long double>(a) * static_cast<long double>(b)) & 0xFu;
            }
        }
        v.updateFmacFlags(laneFlags, dest, extraSticky);
        float *dst = toAcc ? v.m_state.acc : v.m_state.vf[fd];
        for (uint32_t c = 0; c < 4u; ++c)
            if ((dest & laneForComponent(c)) != 0u)
                dst[c] = result[c];
        return true;
    };
    static const VuNativeOps vuNativeOps = {
        [](VU1Interpreter &v) { v.commitReadyPipelines(); },
        [](VU1Interpreter &v, uint64_t t) { v.advanceTo(t); },
        [](VU1Interpreter &v) { v.advanceOneCycle(); },
        [](VU1Interpreter &v, uint32_t u)
        {
            static const bool hlCheck = []() { const char *e = std::getenv("PS2X_HL_CHECK"); return e && e[0] == '1'; }();
            if (!hlCheck)
            {
                if (!fastUpper(v, u)) v.execUpper(u);
                return;
            }
            const VU1State s0 = v.m_state;
            const auto f0 = v.m_flagPipeline;
            PipeMasks &pmv = s_pipeMasks[v.m_unit == VU1Interpreter::Unit::VU1 ? 1 : 0];
            const uint32_t pm0 = pmv.flag;
            const bool took = fastUpper(v, u);
            if (!took) { v.execUpper(u); return; }
            const VU1State s1 = v.m_state;
            const auto f1 = v.m_flagPipeline;
            const uint32_t pm1 = pmv.flag;
            v.m_state = s0; v.m_flagPipeline = f0; pmv.flag = pm0;
            v.execUpper(u);
            const auto flagsEq = [&](const decltype(f1) &x, const decltype(f1) &y) { for (size_t k = 0; k < x.size(); ++k) if (x[k].valid != y[k].valid || (x[k].valid && (x[k].mac != y[k].mac || x[k].status != y[k].status || x[k].extraSticky != y[k].extraSticky || x[k].clip != y[k].clip || x[k].readyCycle != y[k].readyCycle || x[k].issueCycle != y[k].issueCycle || x[k].writesMac != y[k].writesMac || x[k].writesStatus != y[k].writesStatus || x[k].writesSticky != y[k].writesSticky || x[k].writesClip != y[k].writesClip))) return false; return true; };
            static int shown = 0;
            const bool same = std::memcmp(s1.vf, v.m_state.vf, sizeof(s1.vf)) == 0 && std::memcmp(s1.acc, v.m_state.acc, sizeof(s1.acc)) == 0 &&
                              pm1 == pmv.flag && flagsEq(f1, v.m_flagPipeline);
            if (!same && shown < 12)
            {
                ++shown;
                std::printf("[probe] hl DIFF upper=%08X dest=%X fs=%u ft=%u fd=%u pmF=%X/%X", u, (u >> 21) & 0xF, (u >> 11) & 0x1F, (u >> 16) & 0x1F, (u >> 6) & 0x1F, pm1, pmv.flag);
                const uint32_t fd = (u >> 6) & 0x1F;
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t a, b, aa, ba;
                    std::memcpy(&a, &s1.vf[fd][c], 4); std::memcpy(&b, &v.m_state.vf[fd][c], 4);
                    std::memcpy(&aa, &s1.acc[c], 4); std::memcpy(&ba, &v.m_state.acc[c], 4);
                    std::printf(" | c%d vd %08X/%08X acc %08X/%08X", c, a, b, aa, ba);
                }
                std::printf(" flagEq=%d", (int)flagsEq(f1, v.m_flagPipeline));
                for (size_t k = 0; k < f1.size(); ++k)
                    if (f1[k].valid != v.m_flagPipeline[k].valid || (f1[k].valid && (f1[k].mac != v.m_flagPipeline[k].mac || f1[k].status != v.m_flagPipeline[k].status || f1[k].extraSticky != v.m_flagPipeline[k].extraSticky)))
                        std::printf(" | slot%zu fast mac=%X st=%X xs=%X orig mac=%X st=%X xs=%X", k, f1[k].mac, f1[k].status, f1[k].extraSticky, v.m_flagPipeline[k].mac, v.m_flagPipeline[k].status, v.m_flagPipeline[k].extraSticky);
                std::printf("\n");
            }
        },
        [](VU1Interpreter &v, uint32_t l, uint8_t *d, uint32_t ds, GS &g, PS2Memory *m, uint32_t u) { v.execLower(l, d, ds, g, m, u); },
        [](VU1Interpreter &v, uint8_t r, uint8_t lanes, const float *val, uint32_t lat) { v.queueVfWrite(r, lanes, val, lat); },
        [](VU1Interpreter &v, uint8_t r, int32_t val, uint32_t lat) { v.queueViWrite(r, val, lat); },
        [](VU1Interpreter &v, uint8_t lanes, const float *val, uint32_t lat) { v.queueAccWrite(lanes, val, lat); },
    };
    const bool useFastUpper = !s_vfyRef && !vu1FastMode(); // PERF-HM: interpreter loop (VU0 + non-native VU1) uses fastUpper too
    static const bool vuNativeAllowed = []() { const char *e = std::getenv("PS2X_VU1_NATIVE"); return !(e && e[0] == '0'); }();
    const bool vuNativeOn = vuNativeAllowed && (m_unit == Unit::VU1) && !s_vfyRef && decodeCacheOk && !vu1FastMode() &&
                            s_hcCurHash == kVu1NativeMainHash && codeSize == 0x4000u && !isVu3dProbeEnabled() &&
                            !probe_ae::isEnabled() && !probe_ae::isDcProbeEnabled() && !s_egCurRunTraced &&
                            !s_emRecording && !s_eiCurRunActive && !s_ekCurRunActive && !s_fcC80.active;
    VuNativeCtx vuNativeCtx{&vuNativeOps, budgetEnd, 0u, false, vuData, dataSize, &gs, memory, codeSize};

    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        if (vuNativeOn)
        {
            const int nativeResult = vu1NativeRun(*this, vuNativeCtx);
            instrsThisRun += vuNativeCtx.instrs;
            vuNativeCtx.instrs = 0u;
            if (vuNativeCtx.programEnded)
                programEnded = true;
            if (nativeResult == 1)
                break;
            // nativeResult == 2: this pair has no native executor -> interpret it below
        }
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        if (s_fcC80.active && (m_state.pc < 0xC80u || m_state.pc > 0xCB8u))
        {
            processFcC80Completion(vuData, dataSize);
        }

        if (m_state.pc == 0xC80u && isVu3dProbeEnabled())
        {
            if (s_fcC80.active)
            {
                processFcC80Completion(vuData, dataSize);
            }

            uint32_t vi13Val = static_cast<uint32_t>(m_state.vi[13]);
            uint32_t vi12Val = static_cast<uint32_t>(m_state.vi[12]);
            uint32_t qwIdx = (vi13Val - 1u) & 0x3FFu;
            uint32_t qwWords[4]{};
            if (vuData && (qwIdx + 1u) * 16u <= dataSize)
            {
                std::memcpy(qwWords, vuData + qwIdx * 16u, 16);
            }
            char evBuf[128];
            std::snprintf(evBuf, sizeof(evBuf), "C80 vi13=%u vi12=%u end=0x%X start=0x%X",
                          vi13Val, vi12Val, qwWords[1], qwWords[2]);
            ps2x_probeEvent(evBuf);

            uint32_t dst = qwWords[2];
            uint32_t end = qwWords[1];
            uint32_t copied = (end >= dst) ? (end - dst) : 0u;

            s_fcC80.active = true;
            s_fcC80.vi13 = vi13Val;
            s_fcC80.dst = dst;
            s_fcC80.copied = copied;
            s_fcC80.top = s_dyCurTop;
            s_fcC80.mscal_pc = s_dyCurStartPc;

            if (vi13Val == 235u)
            {
                uint32_t vw10[10][4]{};
                bool hasBad = false;
                for (uint32_t i = 0; i < 10u; ++i)
                {
                    if (vuData && (235u + i + 1u) * 16u <= dataSize)
                    {
                        std::memcpy(vw10[i], vuData + (235u + i) * 16u, 16);
                        for (int l = 0; l < 4; ++l)
                        {
                            uint32_t w = vw10[i][l];
                            if (w == 0x7F7FFFFFu || w == 0xFF7FFFFFu || w == 0xFF7FFFFEu)
                            {
                                hasBad = true;
                                break;
                            }
                            float f = 0.0f;
                            std::memcpy(&f, &w, 4);
                            if (std::isnan(f) || std::isinf(f))
                            {
                                hasBad = true;
                                break;
                            }
                        }
                    }
                    if (hasBad) break;
                }

                if (hasBad)
                {
                    static std::atomic<uint32_t> s_evBadPrintCount{0};
                    uint32_t bIdx = s_evBadPrintCount.fetch_add(1, std::memory_order_relaxed);
                    if (bIdx < 3u)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        uint64_t ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - s_vdStartTime).count());
                        std::printf("[probe] ev bad hit #%u at ms=%llu\n", bIdx + 1u, (unsigned long long)ms);
                        for (uint32_t i = 0; i < 10u; ++i)
                        {
                            std::printf("[probe] ev vu10 i=%u %08X %08X %08X %08X\n",
                                        i, vw10[i][0], vw10[i][1], vw10[i][2], vw10[i][3]);
                        }
                        ps2_vif1_dumpEvBuffers();
                        std::fflush(stdout);
                    }
                }
            }
        }

        float preVf[32][4];
        int32_t preVi[16];
        if (s_egCurRunTraced || s_emRecording)
        {
            std::memcpy(preVf, m_state.vf, sizeof(preVf));
            std::memcpy(preVi, m_state.vi, sizeof(preVi));
        }
        const DecodedInstructionPair *decodedPtr;
        if (!s_vfyRef && decodeCacheOk && (m_state.pc & 7u) == 0u && (m_state.pc >> 3) < kMaxDecodedPairs)
        {
            decodedPtr = &m_decodedCodeCache[m_state.pc >> 3]; // PERF-GR: no per-pair revalidation / ~250B struct copy
        }
        else
        {
            decodedFallback = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
            decodedPtr = &decodedFallback;
        }
        const DecodedInstructionPair &decoded = *decodedPtr;
        if (probe_ae::isDcProbeEnabled()) { uint32_t idx = s_dyLastPcIdx++; s_dyLastPcs[idx & 4095u] = m_state.pc; }
        instrsThisRun++;
        { static uint64_t *s_h = new uint64_t[512](); s_geHistPtr = s_h; s_h[(m_state.pc >> 6) & 0x1FFu]++; } // PROBE-GE2
        if (probe_ae::isEnabled())
        {
            probe_ae::onBaInstruction(m_state.pc, decoded.upper, decoded.lower);
        }
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        const auto fastReadyCycle = [&]() -> uint64_t
        {
            uint64_t ready = m_cycle;
            if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
                ready = std::max(ready, m_fdiv.readyCycle);
            if (decoded.lowerUsage.pipeline == PipelineEfu)
                ready = std::max(ready, m_efuResourceReady);
            if (decoded.lowerUsage.waitQ && m_fdiv.valid)
                ready = std::max(ready, m_fdiv.readyCycle);
            if (decoded.lowerUsage.waitP)
            {
                for (const ScalarPipelineEntry &entry : m_efu)
                    if (entry.valid)
                        ready = std::max(ready, entry.readyCycle);
            }
            if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
                ready = std::max(ready, m_cycle + 1u);
            return ready;
        };
        uint64_t readyCycle = vu1FastMode() ? fastReadyCycle() : calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = vu1FastMode() ? fastReadyCycle() : calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        if (s_vfyRef)
        {
            for (uint32_t reg = 1; reg < 16u; ++reg)
            {
                if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
                {
                    writtenVi = static_cast<uint8_t>(reg);
                    oldVi = m_state.vi[reg];
                    break;
                }
            }
        }
        else if ((static_cast<uint32_t>(decoded.lowerUsage.viWrite) & 0xFFFEu) != 0u)
        {
            writtenVi = static_cast<uint8_t>(pmCtz(static_cast<uint32_t>(decoded.lowerUsage.viWrite) & 0xFFFEu));
            oldVi = m_state.vi[writtenVi];
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (s_eiCurRunActive)
        {
            s_eiCurRunPairs++;
            if (s_eiLastPairPc == 0x038u)
            {
                s_eiCurRunDisp.push_back(m_state.pc);
            }
            s_eiLastPairPc = m_state.pc;
            EiMemOp mop = decodeEiMem(decoded.lower, m_state, vuData, dataSize);
            uint32_t ringIdx = s_eiRingHead % 160u;
            s_eiRing[ringIdx] = {m_state.pc, decoded.upper, decoded.lower, mop.qw, mop.firstLane, mop.isStore};
            s_eiRingHead = (s_eiRingHead + 1u) % 160u;
            if (s_eiRingCount < 160u) s_eiRingCount++;

            if (!s_eiReadDone && !mop.isStore && mop.qw >= 636 && mop.qw < 700)
            {
                uint32_t opHi = (decoded.lower >> 25) & 0x7Fu;
                uint32_t special = (decoded.lower & 3u) | ((decoded.lower >> 4) & 0x7Cu);
                bool isLq = (opHi == 0x00u) || ((opHi >= 0x40u || (decoded.lower & 0x80000000u)) && (special == 0x34u || special == 0x36u));
                if (isLq && vuData && (mop.qw + 1) * 16 <= static_cast<int32_t>(dataSize))
                {
                    uint32_t lwords[4];
                    std::memcpy(lwords, vuData + mop.qw * 16, 16);
                    bool isGarbage = false;
                    for (int lane = 0; lane < 4; ++lane)
                    {
                        uint32_t w = lwords[lane];
                        if (w == 0x7F7FFFFFu || w == 0xFF7FFFFFu) {
                            isGarbage = true;
                            break;
                        }
                        float f = 0.0f;
                        std::memcpy(&f, &w, 4);
                        if (std::isnan(f) || std::isinf(f) || std::fabs(f) > 1.0e8f) {
                            isGarbage = true;
                            break;
                        }
                    }
                    if (isGarbage)
                    {
                        s_eiReadDone = true;
                        const auto now = std::chrono::steady_clock::now();
                        uint32_t procSec = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - s_vdStartTime).count());
                        dumpEiDiagnostics("read", m_state.pc, static_cast<uint32_t>(mop.qw), lwords, procSec, true, m_state, m_cycle);
                    }
                }
            }
        }
        if (s_emCurRunCandidate)
        {
            s_emCurRunPairs++;
            if (s_emLastPairPc == 0x038u)
            {
                s_emCurRunTargets.push_back(m_state.pc);
                if (s_emCurRunTargets.size() == 1u && s_emCurRunTargets[0] != 0xC80u)
                {
                    s_emRecording = false;
                }
                else if (s_emCurRunTargets.size() == 2u && s_emCurRunTargets[1] != 0x070u)
                {
                    s_emRecording = false;
                }
            }
            s_emLastPairPc = m_state.pc;
        }
        s_elCurRunPairs++;
        if (isVu3dProbeEnabled())
        {
            uint32_t opHi = (decoded.lower >> 25) & 0x7Fu;
            uint32_t special = (decoded.lower & 3u) | ((decoded.lower >> 4) & 0x7Cu);
            bool isLq = (opHi == 0x00u) || ((opHi >= 0x40u || (decoded.lower & 0x80000000u)) && (special == 0x34u || special == 0x36u));
            if (isLq)
            {
                EiMemOp mop = decodeEiMem(decoded.lower, m_state, vuData, dataSize);
                if (mop.qw >= 636 && mop.qw < 700)
                {
                    uint32_t lq = static_cast<uint32_t>(mop.qw);
                    s_elCurRunRdCount++;
                    if (lq < s_elCurRunRdMinQw) s_elCurRunRdMinQw = lq;
                    if (lq > s_elCurRunRdMaxQw) s_elCurRunRdMaxQw = lq;

                    if (vuData && (lq + 1u) * 16u <= dataSize) {
                        uint32_t lw[4];
                        std::memcpy(lw, vuData + lq * 16u, 16);
                        if (lw[0] == 0 && lw[1] == 0 && lw[2] == 0 && lw[3] == 0) {
                            s_elCurRunRdZero++;
                        }
                    }

                    const auto &elW = s_elLastWriter[lq - 636u];
                    if (!elW.written) {
                        s_elCurRunRdNever++;
                    } else {
                        uint64_t age = (s_elCurRunId >= elW.runId) ? (s_elCurRunId - elW.runId) : 0u;
                        if (age < s_elCurRunRdMinAge) s_elCurRunRdMinAge = age;
                        if (age > s_elCurRunRdMaxAge) s_elCurRunRdMaxAge = age;
                        s_elCurRunHaveAge = true;
                    }
                }
            }
        }
        if (s_ekCurRunActive)
        {
            s_ekCurRunPairs++;
            if (s_ekLastPairPc == 0x038u)
            {
                s_ekCurRunTargets.push_back(m_state.pc);
            }
            s_ekLastPairPc = m_state.pc;
        }
        s_ehCurLowerInstr = decoded.lower;
        if (decoded.iBit)
        {
            if (!(useFastUpper && fastUpper(*this, decoded.upper))) execUpper(decoded.upper); // PERF-HM
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            if (!(useFastUpper && fastUpper(*this, decoded.upper))) execUpper(decoded.upper); // PERF-HM
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            if (!(useFastUpper && fastUpper(*this, decoded.upper))) execUpper(decoded.upper); // PERF-HM
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        if (vu1FastMode())
        {
            // PERF-GJ: results already landed in m_state; no restore/queue/hazard bookkeeping
        }
        else
        {
            if (hasUpperWrite)
            {
                std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
                std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
                const uint32_t latency =
                    decoded.upperUsage.vfLatency != 0u
                        ? decoded.upperUsage.vfLatency
                        : decoded.upperUsage.latency;
                queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
            }
            if (hasDistinctLowerWrite)
            {
                std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
                std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
                const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                             ? decoded.lowerUsage.vfLatency
                                             : decoded.lowerUsage.latency;
                queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
            }
            if (decoded.upperUsage.accWrite != 0u)
            {
                std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
                std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
                // ACC is forwarded to the next upper instruction. Its arithmetic
                // flags still use the normal four-cycle FMAC timeline.
                queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                              kAccForwardLatency);
            }
            if (writtenVi != 0u)
            {
                const int32_t newVi = m_state.vi[writtenVi];
                m_state.vi[writtenVi] = oldVi;
                const uint32_t latency =
                    decoded.lowerUsage.viLatency != 0u
                        ? decoded.lowerUsage.viLatency
                        : decoded.lowerUsage.latency;
                queueViWrite(writtenVi, newVi, latency);
            }

            markPairWrites(decoded);
        }
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (s_egCurRunTraced)
        {
            s_egTotalPairs++;
            if (s_egTraceCount < 700u)
            {
                auto &t = s_egTraces[s_egTraceCount++];
                t.pc = m_state.pc;
                t.upper = decoded.upper;
                t.lower = decoded.lower;
                t.numVf = 0;
                t.numVi = 0;
                for (int r = 0; r < 32; ++r) {
                    if (std::memcmp(preVf[r], m_state.vf[r], 16) != 0) {
                        if (t.numVf < 4) {
                            auto &ch = t.vfChanges[t.numVf++];
                            ch.reg = static_cast<uint8_t>(r);
                            std::memcpy(ch.words, m_state.vf[r], 16);
                        }
                    }
                }
                for (int r = 0; r < 16; ++r) {
                    if (preVi[r] != m_state.vi[r]) {
                        if (t.numVi < 4) {
                            auto &ch = t.viChanges[t.numVi++];
                            ch.reg = static_cast<uint8_t>(r);
                            ch.val = m_state.vi[r];
                        }
                    }
                }
                std::memcpy(&t.qHex, &m_state.q, 4);
                std::memcpy(&t.pHex, &m_state.p, 4);
                t.mac = m_state.mac;
                t.status = m_state.status;
                t.clip = m_state.clip;
            }
        }
        if (s_emRecording)
        {
            if (decoded.iBit) {
                s_emLowOpHist["I_IMM"]++;
            } else {
                s_emLowOpHist[getLowerOpName(decoded.lower)]++;
            }

            EgPairTrace t{};
            t.pc = m_state.pc;
            t.upper = decoded.upper;
            t.lower = decoded.lower;
            t.numVf = 0;
            t.numVi = 0;
            for (int r = 0; r < 32; ++r) {
                if (std::memcmp(preVf[r], m_state.vf[r], 16) != 0) {
                    if (t.numVf < 4) {
                        auto &ch = t.vfChanges[t.numVf++];
                        ch.reg = static_cast<uint8_t>(r);
                        std::memcpy(ch.words, m_state.vf[r], 16);
                    }
                }
            }
            for (int r = 0; r < 16; ++r) {
                if (preVi[r] != m_state.vi[r]) {
                    if (t.numVi < 4) {
                        auto &ch = t.viChanges[t.numVi++];
                        ch.reg = static_cast<uint8_t>(r);
                        ch.val = m_state.vi[r];
                    }
                }
            }
            std::memcpy(&t.qHex, &m_state.q, 4);
            std::memcpy(&t.pHex, &m_state.p, 4);
            t.mac = m_state.mac;
            t.status = m_state.status;
            t.clip = m_state.clip;

            if (s_emTraceCount < 1000u) {
                s_emFirstTraces[s_emTraceCount] = t;
            }
            s_emTailRing[s_emTraceCount % 1000u] = t;
            s_emTraceCount++;
        }
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    if (s_emCurRunCandidate)
    {
        s_emCurRunCandidate = false;
        bool has070 = false, has1EE0 = false;
        for (uint32_t tgt : s_emCurRunTargets) {
            if (tgt == 0x070u) has070 = true;
            if (tgt == 0x1EE0u) has1EE0 = true;
        }

        if (!s_emDone && s_emRecording && has070 && has1EE0 && s_vdCurRunVerts >= 15u && ++s_fwSkip > 150u)
        {
            s_emDone = true;
            s_emRecording = false;
            // Task FW: raw VU1 data memory at run entry / exit for offline re-simulation
            if (FILE *fe = std::fopen("logs/vu1_entry.bin", "wb")) { std::fwrite(s_emEntryVuData, 1, sizeof(s_emEntryVuData), fe); std::fclose(fe); }
            if (FILE *fx = std::fopen("logs/vu1_exit.bin", "wb"))
            {
                if (m_activeVuData) std::fwrite(m_activeVuData, 1, m_activeVuDataSize < 16384u ? m_activeVuDataSize : 16384u, fx);
                std::fclose(fx);
            }

            std::printf("[probe] en run_verts=%u zero_xy=%u pairs=%u\n",
                        s_vdCurRunVerts, s_vdCurRunZeroXy, s_emCurRunPairs);

            for (int n = 0; n < 32; ++n) {
                uint32_t w[4];
                std::memcpy(w, s_emEntryVf[n], 16);
                std::printf("[probe] en entry vf%02d=%08X %08X %08X %08X\n", n, w[0], w[1], w[2], w[3]);
            }
            std::string viStr;
            for (int r = 0; r < 16; ++r) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%s%d", (r > 0 ? " " : ""), s_emEntryVi[r]);
                viStr += buf;
            }
            uint32_t qHex = 0, pHex = 0;
            std::memcpy(&qHex, &s_emEntryQ, 4);
            std::memcpy(&pHex, &s_emEntryP, 4);
            std::printf("[probe] en entry vi=%s top=%u itop=%u q=0x%08X p=0x%08X\n",
                        viStr.c_str(), s_emEntryTop, s_emEntryItop, qHex, pHex);

            // Task FE: VU1 data table (entry snapshot) with last-writer tags
            {
                static const char *const kFeKind[3] = {"NONE", "UNPACK", "VUSTORE"};
                std::lock_guard<std::mutex> feLock(s_feWriterMutex);
                for (uint32_t qw = 440u; qw < 1024u; ++qw)
                {
                    const bool inTbl = (qw <= 520u) || (qw >= 636u && qw <= 698u) || (qw >= 716u && qw <= 778u);
                    const bool inConst = (qw >= 1002u);
                    if (!inTbl && !inConst)
                        continue;
                    const uint32_t *d = s_emEntryVuData[qw];
                    if (inConst)
                    {
                        std::printf("[probe] fe const qw=%u %08X %08X %08X %08X\n", qw, d[0], d[1], d[2], d[3]);
                        continue;
                    }
                    const FeWriterTag &w = s_feWriter[qw];
                    std::printf("[probe] fe tbl qw=%u %08X %08X %08X %08X writer=%s/ev%u/dest%u/num%u/src0x%08X/pc%X\n",
                                qw, d[0], d[1], d[2], d[3], kFeKind[static_cast<uint32_t>(w.kind) & 3u], w.ev,
                                static_cast<uint32_t>(w.dest), static_cast<uint32_t>(w.num), w.src_ea, w.pc);
                }
                std::fflush(stdout);
            }

            auto printTrace =[](const EgPairTrace &t) {
                std::string vfwStr;
                for (uint8_t v = 0; v < t.numVf; ++v) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%svf%02u:%08X %08X %08X %08X", (v > 0 ? ";" : ""), t.vfChanges[v].reg,
                                  t.vfChanges[v].words[0], t.vfChanges[v].words[1], t.vfChanges[v].words[2], t.vfChanges[v].words[3]);
                    vfwStr += buf;
                }
                std::string viwStr;
                for (uint8_t v = 0; v < t.numVi; ++v) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%svi%02u:%d", (v > 0 ? ";" : ""), t.viChanges[v].reg, t.viChanges[v].val);
                    viwStr += buf;
                }
                std::printf("[probe] en pc=0x%03X up=0x%08X lo=0x%08X", t.pc, t.upper, t.lower);
                if (!vfwStr.empty()) std::printf(" vfw=%s", vfwStr.c_str());
                if (!viwStr.empty()) std::printf(" viw=%s", viwStr.c_str());
                std::printf(" q=0x%08X mac=0x%08X st=0x%08X clip=0x%08X\n", t.qHex, t.mac, t.status, t.clip);
            };

            if (s_emTraceCount <= 2000u)
            {
                uint32_t firstLimit = std::min(s_emTraceCount, 1000u);
                for (uint32_t i = 0; i < firstLimit; ++i) {
                    printTrace(s_emFirstTraces[i]);
                }
                for (uint32_t i = 1000u; i < s_emTraceCount; ++i) {
                    printTrace(s_emTailRing[i % 1000u]);
                }
            }
            else
            {
                for (uint32_t i = 0; i < 1000u; ++i) {
                    printTrace(s_emFirstTraces[i]);
                }
                std::printf("[probe] en pc gap skipped %u pairs (pairs>2000 kept first 1000 + last 1000)\n",
                            s_emTraceCount - 2000u);
                uint32_t tailStart = s_emTraceCount - 1000u;
                for (uint32_t i = 0; i < 1000u; ++i) {
                    printTrace(s_emTailRing[(tailStart + i) % 1000u]);
                }
            }

            // Lower opcode histogram
            std::vector<std::pair<std::string, uint32_t>> sortedOps(s_emLowOpHist.begin(), s_emLowOpHist.end());
            std::sort(sortedOps.begin(), sortedOps.end(), [](const auto &a, const auto &b) {
                return a.second > b.second;
            });
            std::string lowOpStr;
            for (const auto &p : sortedOps) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s%s=%u", (lowOpStr.empty() ? "" : " "), p.first.c_str(), p.second);
                lowOpStr += buf;
            }
            std::printf("[probe] en lowop %s\n", lowOpStr.c_str());

            // 16 qw of VU1 data at vi13 and xtop(vi12)
            auto dump16Qw = [&](const char *tag, uint32_t baseQw) {
                for (uint32_t q = 0; q < 16u; ++q) {
                    uint32_t eff = (baseQw + q) & 0x3FFu;
                    uint32_t w[4]{};
                    if (vuData && (eff + 1u) * 16u <= dataSize) {
                        std::memcpy(w, vuData + eff * 16u, 16);
                    }
                    std::printf("[probe] en data_%s qw=%u off=%u w=%08X %08X %08X %08X\n",
                                tag, eff, q, w[0], w[1], w[2], w[3]);
                }
            };

            dump16Qw("vi13", static_cast<uint32_t>(m_state.vi[13]) & 0x3FFu);
            if ((static_cast<uint32_t>(s_emEntryVi[13]) & 0x3FFu) != (static_cast<uint32_t>(m_state.vi[13]) & 0x3FFu)) {
                dump16Qw("entry_vi13", static_cast<uint32_t>(s_emEntryVi[13]) & 0x3FFu);
            }
            dump16Qw("xtop_vi12", static_cast<uint32_t>(m_state.vi[12]) & 0x3FFu);
            if ((static_cast<uint32_t>(s_emEntryVi[12]) & 0x3FFu) != (static_cast<uint32_t>(m_state.vi[12]) & 0x3FFu)) {
                dump16Qw("entry_xtop_vi12", static_cast<uint32_t>(s_emEntryVi[12]) & 0x3FFu);
            }

            auto checkPrintQw = [](uint32_t q) {
                const auto &w = s_emEntryVuData[q];
                if (w[0] != 0 || w[1] != 0 || w[2] != 0 || w[3] != 0) {
                    std::printf("[probe] en data qw=%u w=%08X %08X %08X %08X\n", q, w[0], w[1], w[2], w[3]);
                }
            };
            for (uint32_t q = 0; q <= 64; ++q) checkPrintQw(q);
            for (uint32_t q = 440; q <= 520; ++q) checkPrintQw(q);
            for (uint32_t q = 600; q <= 720; ++q) checkPrintQw(q);
            for (uint32_t q = 976; q <= 1023; ++q) checkPrintQw(q);

            for (size_t p = 0; p < s_emCurRunPackets.size(); ++p) {
                const auto &pkt = s_emCurRunPackets[p];
                uint32_t limit = std::min<uint32_t>(40u, pkt.qwCount);
                for (uint32_t q = 0; q < limit; ++q) {
                    std::printf("[probe] en out pkt=%u qw=%u w=%08X %08X %08X %08X\n",
                                static_cast<unsigned>(p), q, pkt.words[q][0], pkt.words[q][1], pkt.words[q][2], pkt.words[q][3]);
                }
            }
            std::fflush(stdout);
        }
        s_emRecording = false;
    }
    if (s_fcC80.active && (m_stopRequested || m_state.pc + 8u > codeSize || m_state.pc < 0xC80u || m_state.pc > 0xCB8u))
    {
        processFcC80Completion(vuData, dataSize);
    }
    if (isVu3dProbeEnabled())
    {
        uint32_t spc = (s_dyCurStartPc & 0x7FFFFFFF);
        if (s_ekCurRunActive)
        {
            s_ekCurRunActive = false;
            std::string seqStr;
            for (size_t i = 0; i < s_ekCurRunTargets.size(); ++i) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%s%03X", (i > 0 ? "," : ""), s_ekCurRunTargets[i]);
                seqStr += buf;
            }
            if (seqStr.empty()) seqStr = "NONE";

            auto &st = s_ekSeqStats[seqStr];
            st.runs++;
            st.totalPairs += s_ekCurRunPairs;
            st.verts += s_vdCurRunVerts;
            st.zeroXy += s_vdCurRunZeroXy;
            st.st636 += s_ekCurRunSt636;
            st.st636Zero += s_ekCurRunSt636Zero;

            for (uint32_t tgt : s_ekCurRunTargets) {
                s_ekCmdHist[tgt]++;
            }

            bool isDrawRun = (seqStr.find("1EE0") != std::string::npos);
            if (!s_elWindowOpened && isDrawRun && s_vdCurRunZeroXy >= 1u)
            {
                s_elWindowOpened = true;
                s_elWindowRunsRemaining = 400u;
            }
            if (s_elWindowOpened && s_elWindowRunsRemaining > 0u)
            {
                if (isDrawRun && s_vdCurRunZeroXy > 0u && s_elDrawHdrs.size() < 6u)
                {
                    ElDrawHdr dh;
                    dh.runId = s_elCurRunId;
                    dh.top = s_dyCurTop;
                    for (uint32_t q = 0; q < 12u; ++q) {
                        uint32_t effQw = (s_dyCurTop + q) & 0x3FFu;
                        if (vuData && (effQw + 1u) * 16u <= dataSize) {
                            std::memcpy(dh.words[q], vuData + effQw * 16u, 16);
                        }
                    }
                    s_elDrawHdrs.push_back(dh);
                }

                char stBuf[64], rdBuf[64], ageBuf[64];
                if (s_elCurRunStCount > 0u) {
                    std::snprintf(stBuf, sizeof(stBuf), "[%u,%u]x%u", s_elCurRunStMinQw, s_elCurRunStMaxQw, s_elCurRunStCount);
                } else {
                    std::snprintf(stBuf, sizeof(stBuf), "[0,0]x0");
                }
                if (s_elCurRunRdCount > 0u) {
                    std::snprintf(rdBuf, sizeof(rdBuf), "[%u,%u]x%u", s_elCurRunRdMinQw, s_elCurRunRdMaxQw, s_elCurRunRdCount);
                } else {
                    std::snprintf(rdBuf, sizeof(rdBuf), "[0,0]x0");
                }
                if (s_elCurRunHaveAge) {
                    std::snprintf(ageBuf, sizeof(ageBuf), "%llu..%llu", (unsigned long long)s_elCurRunRdMinAge, (unsigned long long)s_elCurRunRdMaxAge);
                } else {
                    std::snprintf(ageBuf, sizeof(ageBuf), "none");
                }

                std::printf("[probe] el run=%llu start=0x%03X seq=<%s> pairs=%u verts=%u zero_xy=%u st=%s rd=%s rd_zero=%u rd_never=%u rd_age=%s top=%u itop=%u\n",
                            (unsigned long long)s_elCurRunId, spc, seqStr.c_str(), s_elCurRunPairs,
                            s_vdCurRunVerts, s_vdCurRunZeroXy, stBuf, rdBuf,
                            s_elCurRunRdZero, s_elCurRunRdNever, ageBuf, s_dyCurTop, s_dyCurItop);
                s_elWindowRunsRemaining--;
                if (s_elWindowRunsRemaining == 0u && !s_elHdrsDumped) {
                    s_elHdrsDumped = true;
                    for (const auto &h : s_elDrawHdrs) {
                        for (uint32_t q = 0; q < 12u; ++q) {
                            std::printf("[probe] el hdr run=%llu qw=%u w=%08X %08X %08X %08X\n",
                                        (unsigned long long)h.runId, q, h.words[q][0], h.words[q][1], h.words[q][2], h.words[q][3]);
                        }
                    }
                }
                std::fflush(stdout);
            }
            if (s_ekSeenHdrSequences.size() < 30u && s_ekSeenHdrSequences.insert(seqStr).second) {
                for (uint32_t q = 0; q < 12u; ++q) {
                    uint32_t effQw = (s_dyCurTop + q) & 0x3FFu;
                    const auto &w = s_ekEntryVuData[effQw];
                    std::printf("[probe] ek hdr seq=<%s> qw=%u w=%08X %08X %08X %08X\n",
                                seqStr.c_str(), q, w[0], w[1], w[2], w[3]);
                }
                std::fflush(stdout);
            }
        }
        else if (spc == 0x000u || spc == 0x040u || spc == 0x4B0u || spc == 0x348u)
        {
            auto &o = s_ekOtherStats[spc];
            o.runs++;
            o.cycles += (m_cycle > tRunStartCycle) ? (m_cycle - tRunStartCycle) : 0u;
        }
        if (spc != 0x08u && s_elWindowOpened && s_elWindowRunsRemaining > 0u)
        {
            std::printf("[probe] el run=%llu start=0x%03X pairs=%u\n",
                        (unsigned long long)s_elCurRunId, spc, s_elCurRunPairs);
            s_elWindowRunsRemaining--;
            if (s_elWindowRunsRemaining == 0u && !s_elHdrsDumped) {
                s_elHdrsDumped = true;
                for (const auto &h : s_elDrawHdrs) {
                    for (uint32_t q = 0; q < 12u; ++q) {
                        std::printf("[probe] el hdr run=%llu qw=%u w=%08X %08X %08X %08X\n",
                                    (unsigned long long)h.runId, q, h.words[q][0], h.words[q][1], h.words[q][2], h.words[q][3]);
                    }
                }
            }
            std::fflush(stdout);
        }
    }
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);

    { // PROBE-GE: per-second VU1 time budget
        static uint64_t s_geNs = 0, s_geXgNs = 0, s_geInstr = 0, s_geCalls = 0, s_geCycles = 0;
        static std::chrono::steady_clock::time_point s_geT0 = std::chrono::steady_clock::now();
        const auto geNow = std::chrono::steady_clock::now();
        s_geNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(geNow - tRunStart).count());
        s_geXgNs += s_currentRunXgkickNs;
        s_geInstr += instrsThisRun;
        s_geCycles += (m_cycle > tRunStartCycle) ? (m_cycle - tRunStartCycle) : 0u;
        ++s_geCalls;
        if (geNow - s_geT0 >= std::chrono::seconds(1))
        {
            std::printf("[probe] ge vu1 calls=%llu instrs=%llu cycles=%llu run_ms=%.1f xgkick_ms=%.1f\n",
                        (unsigned long long)s_geCalls, (unsigned long long)s_geInstr, (unsigned long long)s_geCycles,
                        s_geNs / 1e6, s_geXgNs / 1e6);
            std::fflush(stdout);
            if (s_geHistPtr)
            {
                uint64_t tot = 0;
                for (int b = 0; b < 512; ++b) tot += s_geHistPtr[b];
                for (int k = 0; k < 8 && tot; ++k)
                {
                    int best = 0;
                    for (int b = 1; b < 512; ++b) if (s_geHistPtr[b] > s_geHistPtr[best]) best = b;
                    if (s_geHistPtr[best] == 0) break;
                    std::printf("[probe] ge2 img=%016llx pc=0x%04X-0x%04X pct=%.1f\n", static_cast<unsigned long long>(s_hcCurHash), best * 64, best * 64 + 63, 100.0 * s_geHistPtr[best] / tot);
                    s_geHistPtr[best] = 0;
                }
                for (int b = 0; b < 512; ++b) s_geHistPtr[b] = 0;
            }
            s_geNs = s_geXgNs = s_geInstr = s_geCalls = s_geCycles = 0;
            s_geT0 = geNow;
        }
    }

    if (probe_ae::isEnabled() || probe_ae::isMovieHleVerify())
    {
        const auto tRunEnd = std::chrono::steady_clock::now();
        uint64_t totalRunNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tRunEnd - tRunStart).count());
        uint64_t loopNs = (totalRunNs > s_currentRunXgkickNs) ? (totalRunNs - s_currentRunXgkickNs) : 0;
        probe_ae::onBaRunEnd(instrsThisRun, loopNs);
        probe_ae::onBbRunEnd(vuData, dataSize);
    }
    
    if (probe_ae::isDcProbeEnabled()) {
        uint64_t cyclesConsumed = (m_cycle > tRunStartCycle) ? (m_cycle - tRunStartCycle) : 0;
        bool hitCap = (m_cycle >= budgetEnd);
        bool endedEbit = programEnded;
        bool otherEnd = (!hitCap && !endedEbit);

        {
            std::lock_guard<std::mutex> lock(s_dyMutex);
            s_dyCalls++;
            if (s_dyCurIsMscnt) s_dyMscnt++;
            if (endedEbit) s_dyEndedEbit++;
            if (hitCap) s_dyHitCap++;
            if (otherEnd) s_dyOther++;
            s_dyTotalInstrs += instrsThisRun;
            s_dyTotalCycles += cyclesConsumed;
            if (instrsThisRun > s_dyMaxInstrs) s_dyMaxInstrs = instrsThisRun;
            if (cyclesConsumed > s_dyMaxCycles) s_dyMaxCycles = static_cast<uint32_t>(cyclesConsumed);

            auto &pcStat = s_dyPcStats[s_dyCurStartPc];
            pcStat.calls++;
            if (hitCap) pcStat.hitCap++;
            pcStat.totalCycles += cyclesConsumed;
            pcStat.totalInstrs += instrsThisRun;
        }

        uint64_t vsyncTick = 0;
        uint8_t *ram = memory ? memory->getRDRAM() : nullptr;
        if (ram) {
            std::memcpy(&vsyncTick, ram + (0x696FD0u & 0x1FFFFFFFu), 8);
        }

        if (hitCap && vsyncTick > 0 && !s_dyCappedDumped) {
            s_dyCappedDumped = true;
            std::printf("[probe] dc14 capped start=0x%08X top=%u itop=%u\n", s_dyCurStartPc, s_dyCurTop, s_dyCurItop);

            // PC histogram of last 4096 instruction pairs
            std::unordered_map<uint32_t, uint32_t> pcHist;
            for (uint32_t i = 0; i < 4096; ++i) {
                pcHist[s_dyLastPcs[i]]++;
            }
            std::vector<std::pair<uint32_t, uint32_t>> histVec(pcHist.begin(), pcHist.end());
            std::sort(histVec.begin(), histVec.end(), [](const auto &a, const auto &b) {
                return a.second > b.second;
            });

            size_t histLimit = std::min<size_t>(12u, histVec.size());
            for (size_t i = 0; i < histLimit; ++i) {
                std::printf("[probe] dc14 capped_pc pc=0x%08X n=%u\n", histVec[i].first, histVec[i].second);
            }

            // VI registers vi01..vi15 and Q/P
            std::string viStr;
            for (int r = 1; r < 16; ++r) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%s%d", (r > 1 ? " " : ""), m_state.vi[r]);
                viStr += buf;
            }
            std::printf("[probe] dc14 capped_regs vi=%s q=%.4f p=%.4f\n", viStr.c_str(), m_state.q, m_state.p);

            // 8 instruction pairs around hottest PC
            uint32_t hotPc = histVec.empty() ? s_dyCurStartPc : histVec[0].first;
            uint32_t basePc = (hotPc >= 32u) ? (hotPc - 32u) : 0u;
            for (int i = 0; i < 8; ++i) {
                uint32_t pairPc = basePc + i * 8u;
                if (pairPc + 8u <= codeSize) {
                    uint32_t upper = *reinterpret_cast<const uint32_t *>(vuCode + pairPc + 4u);
                    uint32_t lower = *reinterpret_cast<const uint32_t *>(vuCode + pairPc + 0u);
                    std::printf("[probe] dc14 capped_pairs pc=0x%08X upper=0x%08X lower=0x%08X\n", pairPc, upper, lower);
                }
            }
            std::fflush(stdout);
        }
        if (s_dzPendingRun) {
            s_dzPendingRun = false;
            bool logThis = false;
            bool isCapped = hitCap;
            bool isFirstCapped = false;
            {
                std::lock_guard<std::mutex> lock(s_dzMutex);
                if (isCapped) {
                    if (s_dzCappedLogged < 6) {
                        s_dzCappedLogged++;
                        logThis = true;
                    }
                    if (!s_dzFirstCappedDumpDone) {
                        s_dzFirstCappedDumpDone = true;
                        isFirstCapped = true;
                    }
                } else {
                    if (s_dzNormalLogged < 6) {
                        s_dzNormalLogged++;
                        logThis = true;
                    }
                }
            }
            if (logThis) {
                std::string viStr;
                for (int r = 1; r < 16; ++r) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%s%d", (r > 1 ? " " : ""), s_dzPendingVi[r]);
                    viStr += buf;
                }
                std::printf("[probe] dc15 entry kind=%s start=0x%02X top=%u itop=%u vi=%s\n",
                            isCapped ? "capped" : "normal", s_dzPendingStartPc, s_dzPendingTop, s_dzPendingItop, viStr.c_str());
                int32_t baseQw = static_cast<int32_t>(s_dzPendingTop) - 2;
                for (int i = 0; i < 32; ++i) {
                    int32_t q = baseQw + i;
                    uint32_t effQw = (q >= 0) ? (static_cast<uint32_t>(q) & 0x3FFu) : ((static_cast<uint32_t>(q + 1024)) & 0x3FFu);
                    std::printf("[probe] dc15 data qw=%d w=%08X %08X %08X %08X\n",
                                effQw, s_dzPendingDataWords[i][0], s_dzPendingDataWords[i][1],
                                s_dzPendingDataWords[i][2], s_dzPendingDataWords[i][3]);
                }
                std::fflush(stdout);
            }
            if (isFirstCapped) {
                // Dump VU1 code for pairs 0x000..0x200 (64 pairs)
                for (uint32_t pc = 0x000u; pc < 0x200u; pc += 8u) {
                    if (pc + 8u <= codeSize) {
                        uint32_t lower = *reinterpret_cast<const uint32_t *>(vuCode + pc + 0u);
                        uint32_t upper = *reinterpret_cast<const uint32_t *>(vuCode + pc + 4u);
                        std::printf("[probe] dc15 code pc=0x%03X upper=0x%08X lower=0x%08X\n", pc, upper, lower);
                    }
                }
                // Dump VIF1 UNPACK ring
                ps2_vif1_dumpDzRing();
                // Dump VIF1 registers
                if (memory) {
                    const auto &vr = memory->vif1_regs;
                    uint32_t cl = vr.cycle & 0xFFu;
                    uint32_t wl = (vr.cycle >> 8) & 0xFFu;
                    std::printf("[probe] dc15 vif1 cl=%u wl=%u mode=%u mask=0x%08X tops=%u base=%u ofst=%u\n",
                                cl, wl, vr.mode & 3u, vr.mask, vr.tops & 0x3FFu, vr.base & 0x3FFu, vr.ofst & 0x3FFu);
                    std::printf("[probe] dc15 vif1_row col row=%08X %08X %08X %08X col=%08X %08X %08X %08X\n",
                                vr.row[0], vr.row[1], vr.row[2], vr.row[3],
                                vr.col[0], vr.col[1], vr.col[2], vr.col[3]);
                }
                std::fflush(stdout);
            }
        }
    }

    if (isVu3dProbeEnabled())
    {
        uint64_t cyclesConsumed = (m_cycle > tRunStartCycle) ? (m_cycle - tRunStartCycle) : 0;
        bool hitCap = (m_cycle >= budgetEnd);
        const auto now = std::chrono::steady_clock::now();
        const uint64_t procElapsedSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - s_vdStartTime).count());
        {
            std::lock_guard<std::mutex> lock(s_vdMutex);
            auto &st = s_vdStats[s_dyCurStartPc];
            st.runs++;
            if (hitCap) st.capped++;
            st.totalCycles += cyclesConsumed;
            st.kicks += s_vdCurRunKicks;
            st.verts += s_vdCurRunVerts;
            st.vZeroXy += s_vdCurRunZeroXy;
            st.vOob += s_vdCurRunOob;
            for (int p = 0; p < 7; ++p)
                st.primHist[p] += s_vdCurRunPrimHist[p];

            if (!s_eeVuDone && s_dyCurStartPc == 0x08u && procElapsedSec >= 60u &&
                s_vdCurRunVerts >= 30u && s_vdCurRunZeroXy >= 4u)
            {
                s_eeVuDone = true;
                std::string viStr;
                for (int r = 1; r < 16; ++r) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%s%d", (r > 1 ? " " : ""), m_state.vi[r]);
                    viStr += buf;
                }
                std::printf("[probe] ee vu entry vi=%s top=%u itop=%u\n", viStr.c_str(), s_dyCurTop, s_dyCurItop);
                uint32_t zeroCount = 0;
                for (uint32_t qw = 0; qw < 1024u; ++qw) {
                    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                    if (vuData && (qw + 1u) * 16u <= dataSize) {
                        std::memcpy(&w0, vuData + qw * 16u + 0u, 4);
                        std::memcpy(&w1, vuData + qw * 16u + 4u, 4);
                        std::memcpy(&w2, vuData + qw * 16u + 8u, 4);
                        std::memcpy(&w3, vuData + qw * 16u + 12u, 4);
                    }
                    if (w0 == 0 && w1 == 0 && w2 == 0 && w3 == 0) {
                        zeroCount++;
                    } else {
                        if (zeroCount > 0) {
                            std::printf("[probe] ee vu data skipped_zero_qw count=%u\n", zeroCount);
                            zeroCount = 0;
                        }
                        std::printf("[probe] ee vu data qw=%u w=%08X %08X %08X %08X\n", qw, w0, w1, w2, w3);
                    }
                }
                if (zeroCount > 0) {
                    std::printf("[probe] ee vu data skipped_zero_qw count=%u\n", zeroCount);
                }
                for (size_t p = 0; p < s_eeCurRunPackets.size(); ++p) {
                    const auto &pkt = s_eeCurRunPackets[p];
                    for (uint32_t q = 0; q < pkt.qwCount; ++q) {
                        std::printf("[probe] ee vu out pkt=%u qw=%u w=%08X %08X %08X %08X\n",
                                    static_cast<unsigned>(p), q, pkt.words[q][0], pkt.words[q][1], pkt.words[q][2], pkt.words[q][3]);
                    }
                }
                ps2_vif1_dumpDzRingCount(64u, "[probe] ee vu unpack");
                if (memory) {
                    const auto &vr = memory->vif1_regs;
                    uint32_t cl = vr.cycle & 0xFFu;
                    uint32_t wl = (vr.cycle >> 8) & 0xFFu;
                    std::printf("[probe] ee vu vif1 cl=%u wl=%u mode=%u mask=0x%08X tops=%u base=%u ofst=%u\n",
                                cl, wl, vr.mode & 3u, vr.mask, vr.tops & 0x3FFu, vr.base & 0x3FFu, vr.ofst & 0x3FFu);
                    std::printf("[probe] ee vu vif1_row col row=%08X %08X %08X %08X col=%08X %08X %08X %08X\n",
                                vr.row[0], vr.row[1], vr.row[2], vr.row[3],
                                vr.col[0], vr.col[1], vr.col[2], vr.col[3]);
                }
                std::fflush(stdout);
            }

            if (!s_egDone && s_egCurRunTraced && s_vdCurRunVerts >= 30u && s_vdCurRunZeroXy >= 4u)
            {
                s_egDone = true;
                s_egCurRunTraced = false;
                std::printf("[probe] eg run_verts=%u zero_xy=%u total_pairs=%u\n", s_vdCurRunVerts, s_vdCurRunZeroXy, s_egTotalPairs);
                for (int n = 0; n < 32; ++n) {
                    uint32_t xw = 0, yw = 0, zw = 0, ww = 0;
                    std::memcpy(&xw, &s_egEntryVf[n][0], 4);
                    std::memcpy(&yw, &s_egEntryVf[n][1], 4);
                    std::memcpy(&zw, &s_egEntryVf[n][2], 4);
                    std::memcpy(&ww, &s_egEntryVf[n][3], 4);
                    std::printf("[probe] eg entry vf%02d=%08X %08X %08X %08X\n", n, xw, yw, zw, ww);
                }
                std::string viStr;
                for (int r = 1; r < 16; ++r) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%s%d", (r > 1 ? " " : ""), s_egEntryVi[r]);
                    viStr += buf;
                }
                std::printf("[probe] eg entry vi=%s top=%u itop=%u\n", viStr.c_str(), s_egEntryTop, s_egEntryItop);
                for (uint32_t i = 0; i < s_egTraceCount; ++i) {
                    const auto &t = s_egTraces[i];
                    std::string vfwStr;
                    for (uint8_t v = 0; v < t.numVf; ++v) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%svf%02u:%08X %08X %08X %08X", (v > 0 ? ";" : ""), t.vfChanges[v].reg,
                                      t.vfChanges[v].words[0], t.vfChanges[v].words[1], t.vfChanges[v].words[2], t.vfChanges[v].words[3]);
                        vfwStr += buf;
                    }
                    std::string viwStr;
                    for (uint8_t v = 0; v < t.numVi; ++v) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%svi%02u:%d", (v > 0 ? ";" : ""), t.viChanges[v].reg, t.viChanges[v].val);
                        viwStr += buf;
                    }
                    std::printf("[probe] eg pc=0x%03X up=0x%08X lo=0x%08X", t.pc, t.upper, t.lower);
                    if (!vfwStr.empty()) std::printf(" vfw=%s", vfwStr.c_str());
                    if (!viwStr.empty()) std::printf(" viw=%s", viwStr.c_str());
                    std::printf(" q=0x%08X mac=0x%08X st=0x%08X clip=0x%08X\n", t.qHex, t.mac, t.status, t.clip);
                }
                for (uint32_t q = 440u; q <= 720u; ++q) {
                    const auto &w = s_egEntryDataWords[q - 440u];
                    if (w[0] != 0 || w[1] != 0 || w[2] != 0 || w[3] != 0) {
                        std::printf("[probe] eg data qw=%u w=%08X %08X %08X %08X\n", q, w[0], w[1], w[2], w[3]);
                    }
                }
                for (size_t p = 0; p < s_egCurRunPackets.size(); ++p) {
                    const auto &pkt = s_egCurRunPackets[p];
                    for (uint32_t q = 0; q < pkt.qwCount; ++q) {
                        std::printf("[probe] eg out pkt=%u qw=%u w=%08X %08X %08X %08X\n",
                                    static_cast<unsigned>(p), q, pkt.words[q][0], pkt.words[q][1], pkt.words[q][2], pkt.words[q][3]);
                    }
                }
                for (uint32_t i = 0; i < s_ehStoreCount; ++i) {
                    const auto &st = s_ehStores[i];
                    std::printf("[probe] eh store pair=%u pc=0x%03X qw=%u w=%08X %08X %08X %08X kind=%s\n",
                                st.pair, st.pc, st.qw, st.words[0], st.words[1], st.words[2], st.words[3], st.kind);
                }
                ps2_vif1_dumpEhRing();
                ps2_vif1_dumpEhShapes();
                for (uint32_t q = 440u; q <= 720u; ++q) {
                    if (vuData && (q + 1u) * 16u <= dataSize) {
                        uint32_t wCur[4];
                        std::memcpy(wCur, vuData + q * 16u, 16);
                        const auto &wEntry = s_egEntryDataWords[q - 440u];
                        if (std::memcmp(wCur, wEntry, 16) != 0) {
                            std::printf("[probe] eh data_after qw=%u w=%08X %08X %08X %08X\n",
                                        q, wCur[0], wCur[1], wCur[2], wCur[3]);
                        }
                    }
                }
                std::fflush(stdout);
            }
            if (s_dyCurStartPc != 0x08u && s_dyCurStartPc != 0x48u && procElapsedSec >= 60u)
            {
                auto &cand = s_vdCandidateEntries[s_dyCurStartPc];
                if (!cand.valid || s_vdCurRunVerts >= cand.vertCount)
                {
                    cand.valid = true;
                    cand.pc = s_dyCurStartPc;
                    cand.top = s_dyCurTop;
                    cand.vertCount = s_vdCurRunVerts;
                    std::memcpy(cand.vf, m_state.vf, sizeof(cand.vf));
                    std::memcpy(cand.vi, m_state.vi, sizeof(cand.vi));
                    int32_t baseQw = static_cast<int32_t>(s_dyCurTop) - 2;
                    for (int i = 0; i < 26; ++i) {
                        int32_t q = baseQw + i;
                        uint32_t effQw = (q >= 0) ? (static_cast<uint32_t>(q) & 0x3FFu) : ((static_cast<uint32_t>(q + 1024)) & 0x3FFu);
                        if (vuData && (effQw + 1u) * 16u <= dataSize) {
                            std::memcpy(cand.dataWords[i], vuData + effQw * 16u, 16);
                        } else {
                            std::memset(cand.dataWords[i], 0, 16);
                        }
                    }
                    std::memcpy(cand.outWords, s_vdFirstXgkickWords, sizeof(cand.outWords));
                }
            }

            if (s_fcLastSummaryTime == std::chrono::steady_clock::time_point{})
                s_fcLastSummaryTime = now;
            else if (std::chrono::duration_cast<std::chrono::seconds>(now - s_fcLastSummaryTime).count() >= 1)
            {
                s_fcLastSummaryTime = now;
                std::printf("[probe] fc c80 total=%u flagged_nonfinite=%u flagged_big=%u zero_rows=%u\n",
                            s_fcTotal.load(std::memory_order_relaxed),
                            s_fcFlaggedNonfinite.load(std::memory_order_relaxed),
                            s_fcFlaggedBig.load(std::memory_order_relaxed),
                            s_fcZeroRows.load(std::memory_order_relaxed));
                std::fflush(stdout);
            }

            if (s_eoLastCountTime == std::chrono::steady_clock::time_point{})
                s_eoLastCountTime = now;
            else if (std::chrono::duration_cast<std::chrono::seconds>(now - s_eoLastCountTime).count() >= 5)
            {
                s_eoLastCountTime = now;
                std::printf("[probe] eo counts unpack_bad=%u vustore_bad=%u\n",
                            ps2_vif1_getEoUnpackBadCount(), s_eoVuStoreBadCount);
                std::fflush(stdout);
            }
            if (s_vdLastWindowTime == std::chrono::steady_clock::time_point{})
                s_vdLastWindowTime = now;
            else if (std::chrono::duration_cast<std::chrono::seconds>(now - s_vdLastWindowTime).count() >= 3)
            {
                s_vdLastWindowTime = now;
                std::vector<std::pair<uint32_t, VdRunStats>> sortedStats(s_vdStats.begin(), s_vdStats.end());
                std::sort(sortedStats.begin(), sortedStats.end(), [](const auto &a, const auto &b) {
                    return a.second.runs > b.second.runs;
                });
                size_t limit = std::min<size_t>(8u, sortedStats.size());
                for (size_t i = 0; i < limit; ++i) {
                    const auto &p = sortedStats[i];
                    uint64_t avgCyc = p.second.runs ? (p.second.totalCycles / p.second.runs) : 0;
                    std::printf("[probe] vd pc=0x%03X runs=%u capped=%u avg_cyc=%llu kicks=%u verts=%u v_zero_xy=%u v_oob=%u prim_types=tri:%u/strip:%u/fan:%u/sprite:%u/line:%u/point:%u\n",
                                p.first, p.second.runs, p.second.capped, (unsigned long long)avgCyc,
                                p.second.kicks, p.second.verts, p.second.vZeroXy, p.second.vOob,
                                p.second.primHist[3], p.second.primHist[4], p.second.primHist[5],
                                p.second.primHist[6], (p.second.primHist[1] + p.second.primHist[2]), p.second.primHist[0]);
                }
                std::fflush(stdout);
                if (isVu3dProbeEnabled())
                {
                    std::vector<std::pair<std::string, EkSeqStats>> sortedSeqs(s_ekSeqStats.begin(), s_ekSeqStats.end());
                    std::sort(sortedSeqs.begin(), sortedSeqs.end(), [](const auto &a, const auto &b) {
                        return a.second.runs > b.second.runs;
                    });
                    size_t limitSeqs = std::min<size_t>(12u, sortedSeqs.size());
                    for (size_t i = 0; i < limitSeqs; ++i) {
                        const auto &sq = sortedSeqs[i];
                        uint64_t avgPairs = sq.second.runs ? (sq.second.totalPairs / sq.second.runs) : 0;
                        std::printf("[probe] ek seq=<%s> runs=%u avg_pairs=%llu verts=%llu zero_xy=%llu st636=%llu st636_zero=%llu\n",
                                    sq.first.c_str(), sq.second.runs, (unsigned long long)avgPairs,
                                    (unsigned long long)sq.second.verts, (unsigned long long)sq.second.zeroXy,
                                    (unsigned long long)sq.second.st636, (unsigned long long)sq.second.st636Zero);
                    }
                    s_ekSeqStats.clear();

                    for (const auto &cmd : s_ekCmdHist) {
                        std::printf("[probe] ek cmd pc=0x%03X count=%u\n", cmd.first, cmd.second);
                    }
                    s_ekCmdHist.clear();

                    for (const auto &oth : s_ekOtherStats) {
                        uint64_t avgCyc = oth.second.runs ? (oth.second.cycles / oth.second.runs) : 0;
                        std::printf("[probe] ek other pc=0x%03X runs=%u avg_cyc=%llu\n",
                                    oth.first, oth.second.runs, (unsigned long long)avgCyc);
                    }
                    s_ekOtherStats.clear();
                }
                s_vdStats.clear();
            }

            // Check candidate entry dump for first 3 distinct PCs
            if (procElapsedSec >= 60u && s_vdDumpedPcs.size() < 3u)
            {
                for (auto &pair : s_vdCandidateEntries) {
                    if (s_vdDumpedPcs.size() >= 3u) break;
                    uint32_t pc = pair.first;
                    if (s_vdDumpedPcs.count(pc) == 0 && pair.second.valid) {
                        s_vdDumpedPcs.insert(pc);
                        const auto &cand = pair.second;
                        std::printf("[probe] vd entry pc=0x%03X top=%u verts=%u\n", cand.pc, cand.top, cand.vertCount);
                        for (int n = 0; n < 32; ++n) {
                            uint32_t xw = 0, yw = 0, zw = 0, ww = 0;
                            std::memcpy(&xw, &cand.vf[n][0], 4);
                            std::memcpy(&yw, &cand.vf[n][1], 4);
                            std::memcpy(&zw, &cand.vf[n][2], 4);
                            std::memcpy(&ww, &cand.vf[n][3], 4);
                            std::printf("[probe] vd entry vf%02d=%08X %08X %08X %08X\n", n, xw, yw, zw, ww);
                        }
                        std::string viStr;
                        for (int r = 1; r < 16; ++r) {
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%s%d", (r > 1 ? " " : ""), cand.vi[r]);
                            viStr += buf;
                        }
                        std::printf("[probe] vd entry vi=%s\n", viStr.c_str());
                        int32_t baseQw = static_cast<int32_t>(cand.top) - 2;
                        for (int i = 0; i < 26; ++i) {
                            int32_t q = baseQw + i;
                            uint32_t effQw = (q >= 0) ? (static_cast<uint32_t>(q) & 0x3FFu) : ((static_cast<uint32_t>(q + 1024)) & 0x3FFu);
                            std::printf("[probe] vd data qw=%u w=%08X %08X %08X %08X\n", effQw, cand.dataWords[i][0], cand.dataWords[i][1], cand.dataWords[i][2], cand.dataWords[i][3]);
                        }
                        for (int i = 0; i < 14; ++i) {
                            std::printf("[probe] vd out qw=%u w=%08X %08X %08X %08X\n", i, cand.outWords[i][0], cand.outWords[i][1], cand.outWords[i][2], cand.outWords[i][3]);
                        }
                        for (uint32_t codePc = 0x000u; codePc < 0x300u; codePc += 8u) {
                            if (codePc + 8u <= codeSize) {
                                uint32_t lower = *reinterpret_cast<const uint32_t *>(vuCode + codePc + 0u);
                                uint32_t upper = *reinterpret_cast<const uint32_t *>(vuCode + codePc + 4u);
                                std::printf("[probe] vd code pc=0x%03X upper=0x%08X lower=0x%08X\n", codePc, upper, lower);
                            }
                        }
                        std::fflush(stdout);
                    }
                }
            }
        }
    }
    probe_ae::onBxOrigRunEnd();
}
