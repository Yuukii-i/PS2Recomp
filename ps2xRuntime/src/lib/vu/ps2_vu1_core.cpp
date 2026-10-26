#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "probe_ae.h"

#include <chrono>
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <ps2_log.h>

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

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_xgkick = {};
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

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

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
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
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
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
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

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
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
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
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
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
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
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
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
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
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
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;

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

    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
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

    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
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

    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
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
    if (m_activeMemory)
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
    m_xgkick = {};
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
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
        for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
        {
            if ((usage->viRead & (1u << reg)) != 0u)
                ready = std::max(ready, m_viReady[reg]);
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

    for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
    {
        if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
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

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    s_currentRunXgkickNs = 0;
    const auto tRunStart = std::chrono::steady_clock::now();
    const uint64_t tRunStartCycle = m_cycle;
    uint32_t instrsThisRun = 0;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (probe_ae::isDcProbeEnabled()) { uint32_t idx = s_dyLastPcIdx++; s_dyLastPcs[idx & 4095u] = m_state.pc; }
        instrsThisRun++;
        if (probe_ae::isEnabled())
        {
            probe_ae::onBaInstruction(m_state.pc, decoded.upper, decoded.lower);
        }
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                oldVi = m_state.vi[reg];
                break;
            }
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

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
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
            execUpper(decoded.upper);
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
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

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
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);

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
