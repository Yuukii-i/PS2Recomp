// vu0_fast.inc -- Task V0B: fast path for straight-line VU0 FMAC microprograms
// (PS2X_VU0_FAST default ON, =0 disables;PS2X_VU0_FAST_CHECK=1 default OFF).
// Included by ps2_runtime.cpp before PS2Runtime::executeVU0Microprogram. No header edits.

#include <algorithm> // PERF-V0D
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <mutex>

namespace vu0fast
{
constexpr uint32_t kMaxPairs = 32u;
constexpr uint32_t kMaxResolvedPairs = 128u; // PERF-V0D
constexpr uint32_t kMaxCachedPaths = 8u;     // PERF-V0E
constexpr uint32_t kNumSlots = PS2_VU0_CODE_SIZE / 8u; // 512
constexpr uint32_t kMaxCachedProgs = 64u;

enum OpKind : uint8_t
{
    OP_NOP = 0,
    OP_MUL = 1,
    OP_MADD = 2,
    OP_ADD = 3,  // PERF-V0D
    OP_SUB = 4,  // PERF-V0D
    OP_MSUB = 5, // PERF-V0D
    OP_CLIP = 6  // PERF-V0D
};

enum LowerKind : uint8_t // PERF-V0D
{
    LOP_NOP = 0,
    LOP_IADDIU = 1,
    LOP_IAND = 2,
    LOP_IOR = 3,
    LOP_FCGET = 4,
    LOP_DIV = 5,
    LOP_RSQRT = 6,
    LOP_WAITQ = 7,
    LOP_JR = 8,
    LOP_JALR = 9,
    LOP_IBEQ = 10,
    LOP_IBNE = 11
};

enum SrcKind : uint8_t
{
    SRC_VT_BC = 0,
    SRC_Q = 1,
    SRC_I = 2,
    SRC_VT_VEC = 3
};

struct DecodedOp
{
    uint8_t kind;   // OpKind
    uint8_t src;    // SrcKind
    uint8_t bc;     // 0..3
    uint8_t toAcc;  // 0 or 1
    uint8_t dest;   // 4-bit xyzw mask (bit 3 = x, bit 0 = w)
    uint8_t fs;     // 0..31
    uint8_t ft;     // 0..31
    uint8_t fd;     // 0..31
};

struct DecodedLower // PERF-V0D
{
    uint8_t kind; // LowerKind
    uint8_t r1;   // viS or vfS
    uint8_t r2;   // viT or vfT
    uint8_t r3;   // viD, fsf, or ftf
    uint8_t r4;   // ftf (for DIV/RSQRT)
    int16_t imm;
    uint16_t targetSlot; // (targetPC >> 3) & 511 for IBEQ/IBNE
};

struct ResolvedPair // PERF-V0D
{
    DecodedOp upper;
    DecodedLower lower;
    uint16_t pc;
    uint8_t eBit;
};

struct CompiledStep // PERF-V0E
{
    DecodedOp upper;
    DecodedLower lower;
    uint16_t pc;
    uint8_t computeMac;
    uint8_t shadowReg;
    uint8_t backupMask;
    uint8_t saveBackup;
    uint8_t commitQBefore;
    uint8_t commitQAfter;
    uint8_t clipIdx;
    uint8_t sseDestMask;
};

struct CompiledPath // PERF-V0E
{
    uint64_t branchMask = 0ull;
    uint8_t branchCount = 0u;
    uint16_t numSteps = 0u;
    uint16_t endPC = 0u;
    uint8_t commitQAtEnd = 0u;
    uint8_t numClips = 0u;
    uint32_t writeVfMask = 0u;
    uint16_t writeViMask = 0u;
    CompiledStep steps[kMaxResolvedPairs]{};
};

struct FastProg
{
    uint32_t endPC;
    uint8_t numOps;
    bool isStraightFmac; // PERF-V0D
    bool usesQ;          // PERF-V0E
    bool usesI;          // PERF-V0E
    uint16_t readViMask; // PERF-V0F
    uint32_t readVfMask; // PERF-V0E
    uint32_t writeVfMask;// PERF-V0E
    DecodedOp ops[kMaxPairs];
    uint16_t numResolvedPairs;               // PERF-V0D
    uint16_t slotToIdx[kNumSlots];           // PERF-V0D
    ResolvedPair resolved[kMaxResolvedPairs]; // PERF-V0D
    mutable uint8_t numPaths = 0u;           // PERF-V0E
    mutable CompiledPath paths[kMaxCachedPaths]{}; // PERF-V0E
};

struct ClassifierCache
{
    bool initialized = false;
    uint64_t generation = UINT64_MAX;
    const uint8_t *codePtr = nullptr;
    uint64_t imageHash = 0;
    uint8_t state[kNumSlots]{};   // 0 = unknown, 1 = disqualified, 2 = qualified
    uint8_t progIdx[kNumSlots]{};
    uint32_t numProgs = 0;
    FastProg progs[kMaxCachedProgs]{};
};

inline bool isFastEnabled()
{
    static const bool s_on = []()
    {
        const char *e = std::getenv("PS2X_VU0_FAST");
        // REVERSAL: PS2X_VU0_FAST=0 restores the generic path (docs/REVERSAL_MEMO.md)
        return !(e && e[0] == '0');
    }();
    return s_on;
}

inline bool isCheckEnabled()
{
    static const bool s_on = []()
    {
        const char *e = std::getenv("PS2X_VU0_FAST_CHECK");
        return e && e[0] == '1';
    }();
    return s_on;
}

inline bool isAnyVerifyOrProfEnabled()
{
    static const bool s_on = []()
    {
        const char *v = std::getenv("PS2X_VU1_VERIFY");
        return (v && std::atoi(v) != 0); // PROBE-V0P: PCPROF no longer disables the fast path
    }();
    return s_on;
}

inline uint64_t fnv1a64(const uint8_t *data, uint32_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size; ++i)
        h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

inline bool isLowerNoop(uint32_t lower)
{
    if (lower == 0u || lower == 0x8000033Cu)
        return true;
    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    if (opHi != 0x40u)
        return false;
    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct < 0x3Cu)
        return false;
    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    const uint8_t dest = static_cast<uint8_t>((lower >> 21) & 0xFu);
    return special == 0x30u && dest == 0u; // MOVE with dest mask 0
}

inline bool decodeSupportedUpper(uint32_t upper, DecodedOp &out)
{
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    uint8_t e = op;
    bool toAcc = false;
    if (op >= 0x3Cu)
    {
        e = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        toAcc = true;
        if (e == 0x2Fu || e == 0x30u)
        {
            out = {};
            out.kind = OP_NOP;
            return true;
        }
    }

    uint8_t kind = OP_NOP;
    uint8_t src = SRC_VT_BC;
    uint8_t bc = 0;
    if (e >= 0x08u && e <= 0x0Bu)
    {
        kind = OP_MADD;
        src = SRC_VT_BC;
        bc = e & 3u;
    }
    else if (e >= 0x18u && e <= 0x1Bu)
    {
        kind = OP_MUL;
        src = SRC_VT_BC;
        bc = e & 3u;
    }
    else
    {
        switch (e)
        {
        case 0x1Cu: kind = OP_MUL;  src = SRC_Q;      break;
        case 0x1Eu: kind = OP_MUL;  src = SRC_I;      break;
        case 0x21u: kind = OP_MADD; src = SRC_Q;      break;
        case 0x23u: kind = OP_MADD; src = SRC_I;      break;
        case 0x29u: kind = OP_MADD; src = SRC_VT_VEC; break;
        case 0x2Au: kind = OP_MUL;  src = SRC_VT_VEC; break;
        default: return false;
        }
    }

    out.kind = kind;
    out.src = src;
    out.bc = bc;
    out.toAcc = toAcc ? 1u : 0u;
    out.dest = static_cast<uint8_t>((upper >> 21) & 0xFu);
    out.ft = static_cast<uint8_t>((upper >> 16) & 0x1Fu);
    out.fs = static_cast<uint8_t>((upper >> 11) & 0x1Fu);
    out.fd = static_cast<uint8_t>((upper >> 6) & 0x1Fu);
    return true;
}

// PERF-V0D: extended upper decoder for resolved-path executor (ADD/SUB/MUL/MADD/MSUB/CLIP/NOP)
inline bool decodeSupportedUpperResolved(uint32_t upper, DecodedOp &out)
{
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    uint8_t e = op;
    bool toAcc = false;
    if (op >= 0x3Cu)
    {
        e = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        if (e == 0x2Fu || e == 0x30u)
        {
            out = {};
            out.kind = OP_NOP;
            return true;
        }
        if (e == 0x1Fu)
        {
            out = {};
            out.kind = OP_CLIP;
            out.ft = static_cast<uint8_t>((upper >> 16) & 0x1Fu);
            out.fs = static_cast<uint8_t>((upper >> 11) & 0x1Fu);
            return true;
        }
        toAcc = true;
    }

    uint8_t kind = OP_NOP;
    uint8_t src = SRC_VT_BC;
    uint8_t bc = 0;
    if (e <= 0x0Fu)
    {
        static constexpr uint8_t kBcKinds[4] = {OP_ADD, OP_SUB, OP_MADD, OP_MSUB};
        kind = kBcKinds[e >> 2];
        src = SRC_VT_BC;
        bc = e & 3u;
    }
    else if (e >= 0x18u && e <= 0x1Bu)
    {
        kind = OP_MUL;
        src = SRC_VT_BC;
        bc = e & 3u;
    }
    else
    {
        switch (e)
        {
        case 0x1Cu: kind = OP_MUL;  src = SRC_Q;      break;
        case 0x1Eu: kind = OP_MUL;  src = SRC_I;      break;
        case 0x20u: kind = OP_ADD;  src = SRC_Q;      break;
        case 0x21u: kind = OP_MADD; src = SRC_Q;      break;
        case 0x22u: kind = OP_ADD;  src = SRC_I;      break;
        case 0x23u: kind = OP_MADD; src = SRC_I;      break;
        case 0x24u: kind = OP_SUB;  src = SRC_Q;      break;
        case 0x25u: kind = OP_MSUB; src = SRC_Q;      break;
        case 0x26u: kind = OP_SUB;  src = SRC_I;      break;
        case 0x27u: kind = OP_MSUB; src = SRC_I;      break;
        case 0x28u: kind = OP_ADD;  src = SRC_VT_VEC; break;
        case 0x29u: kind = OP_MADD; src = SRC_VT_VEC; break;
        case 0x2Au: kind = OP_MUL;  src = SRC_VT_VEC; break;
        case 0x2Cu: kind = OP_SUB;  src = SRC_VT_VEC; break;
        case 0x2Du: kind = OP_MSUB; src = SRC_VT_VEC; break;
        default: return false;
        }
    }

    out.kind = kind;
    out.src = src;
    out.bc = bc;
    out.toAcc = toAcc ? 1u : 0u;
    out.dest = static_cast<uint8_t>((upper >> 21) & 0xFu);
    out.ft = static_cast<uint8_t>((upper >> 16) & 0x1Fu);
    out.fs = static_cast<uint8_t>((upper >> 11) & 0x1Fu);
    out.fd = static_cast<uint8_t>((upper >> 6) & 0x1Fu);
    return true;
}

// PERF-V0D: lower decoder for resolved-path executor
inline bool decodeSupportedLower(uint32_t lower, DecodedLower &out)
{
    out = {};
    if (isLowerNoop(lower))
    {
        out.kind = LOP_NOP;
        return true;
    }

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t viT = static_cast<uint8_t>((lower >> 16) & 0xFu);
    const uint8_t viS = static_cast<uint8_t>((lower >> 11) & 0xFu);
    const uint8_t viD = static_cast<uint8_t>((lower >> 6) & 0xFu);

    switch (opHi)
    {
    case 0x08u: // IADDIU
        out.kind = LOP_IADDIU;
        out.r1 = viS;
        out.r2 = viT;
        out.imm = static_cast<int16_t>((lower & 0x7FFu) | ((lower >> 10) & 0x7800u));
        return true;
    case 0x1Cu: // FCGET
        out.kind = LOP_FCGET;
        out.r2 = viT;
        return true;
    case 0x24u: // JR
        out.kind = LOP_JR;
        out.r1 = viS;
        return true;
    case 0x25u: // JALR
        out.kind = LOP_JALR;
        out.r1 = viS;
        out.r2 = viT;
        return true;
    case 0x28u: // IBEQ
        out.kind = LOP_IBEQ;
        out.r1 = viS;
        out.r2 = viT;
        out.imm = static_cast<int16_t>(static_cast<int32_t>(lower << 21) >> 21);
        return true;
    case 0x29u: // IBNE
        out.kind = LOP_IBNE;
        out.r1 = viS;
        out.r2 = viT;
        out.imm = static_cast<int16_t>(static_cast<int32_t>(lower << 21) >> 21);
        return true;
    case 0x40u:
    {
        const uint8_t funct = static_cast<uint8_t>(lower & 0x3Fu);
        if (funct == 0x34u) // IAND
        {
            out.kind = LOP_IAND;
            out.r1 = viS;
            out.r2 = viT;
            out.r3 = viD;
            return true;
        }
        if (funct == 0x35u) // IOR
        {
            out.kind = LOP_IOR;
            out.r1 = viS;
            out.r2 = viT;
            out.r3 = viD;
            return true;
        }
        if (funct >= 0x3Cu)
        {
            const uint8_t funct2 = static_cast<uint8_t>((lower & 0x3u) | ((lower >> 4) & 0x7Cu));
            if (funct2 == 0x38u || funct2 == 0x3Au) // DIV / RSQRT
            {
                out.kind = (funct2 == 0x38u) ? LOP_DIV : LOP_RSQRT;
                out.r1 = static_cast<uint8_t>((lower >> 11) & 0x1Fu); // vfS
                out.r2 = static_cast<uint8_t>((lower >> 16) & 0x1Fu); // vfT
                out.r3 = static_cast<uint8_t>((lower >> 21) & 0x3u);  // fsf
                out.r4 = static_cast<uint8_t>((lower >> 23) & 0x3u);  // ftf
                return true;
            }
            if (funct2 == 0x3Bu) // WAITQ
            {
                out.kind = LOP_WAITQ;
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

inline bool isLowerBranch(uint8_t kind) // PERF-V0D
{
    return kind >= LOP_JR && kind <= LOP_IBNE;
}

static constexpr uint8_t kBitRev4[16] = {
    0u, 8u, 4u, 12u, 2u, 10u, 6u, 14u,
    1u, 9u, 5u, 13u, 3u, 11u, 7u, 15u
};
alignas(16) static constexpr int32_t kSseLaneMasks[16][4] = {
    { 0,  0,  0,  0}, {-1,  0,  0,  0}, { 0, -1,  0,  0}, {-1, -1,  0,  0},
    { 0,  0, -1,  0}, {-1,  0, -1,  0}, { 0, -1, -1,  0}, {-1, -1, -1,  0},
    { 0,  0,  0, -1}, {-1,  0,  0, -1}, { 0, -1,  0, -1}, {-1, -1,  0, -1},
    { 0,  0, -1, -1}, {-1,  0, -1, -1}, { 0, -1, -1, -1}, {-1, -1, -1, -1}
};

struct LocalClipRing // PERF-V0E
{
    uint64_t cyc[8]{};
    uint32_t val[8]{};
    uint32_t base = 0u;
    uint8_t head = 0u;
    uint8_t count = 0u;
    void push(uint64_t c, uint32_t v)
    {
        if (count == 8u)
            base = val[head];
        else
            ++count;
        cyc[head] = c;
        val[head] = v;
        head = static_cast<uint8_t>((head + 1u) & 7u);
    }
    uint32_t visible(uint64_t c) const
    {
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t ridx = (head + 7u - k) & 7u;
            if (cyc[ridx] + 4u <= c)
                return val[ridx];
        }
        return base;
    }
};

inline void finalizeCompiledPath(CompiledPath &p) // PERF-V0E
{
    int32_t lastFmac = -1;
    p.writeVfMask = 0u;
    p.writeViMask = 0u;
    for (uint16_t i = 0; i < p.numSteps; ++i)
    {
        CompiledStep &st = p.steps[i];
        st.computeMac = 0u;
        st.saveBackup = 0u;
        st.backupMask = 0u;
        st.sseDestMask = kBitRev4[st.upper.dest];
        if (st.upper.kind != OP_NOP && st.upper.kind != OP_CLIP && st.upper.dest != 0u)
        {
            lastFmac = static_cast<int32_t>(i);
            if (!st.upper.toAcc && st.upper.fd != 0u)
                p.writeVfMask |= (1u << st.upper.fd);
        }
        if (st.lower.kind == LOP_IBEQ || st.lower.kind == LOP_IBNE)
        {
            if (lastFmac >= 0)
                p.steps[lastFmac].computeMac = 1u;
            if (i > 0u)
            {
                const DecodedLower &prev = p.steps[i - 1u].lower;
                uint8_t wReg = 0u;
                if (prev.kind == LOP_IADDIU) wReg = prev.r2;
                else if (prev.kind == LOP_IAND || prev.kind == LOP_IOR) wReg = prev.r3;
                if (wReg != 0u)
                {
                    if (st.lower.r1 == wReg) st.backupMask |= 1u;
                    if (st.lower.r2 == wReg) st.backupMask |= 2u;
                    if (st.backupMask != 0u) p.steps[i - 1u].saveBackup = 1u;
                }
            }
        }
        if (st.lower.kind == LOP_IADDIU || st.lower.kind == LOP_FCGET || st.lower.kind == LOP_JALR)
        {
            if (st.lower.r2 != 0u)
                p.writeViMask |= static_cast<uint16_t>(1u << st.lower.r2);
        }
        else if (st.lower.kind == LOP_IAND || st.lower.kind == LOP_IOR)
        {
            if (st.lower.r3 != 0u)
                p.writeViMask |= static_cast<uint16_t>(1u << st.lower.r3);
        }
    }
    if (lastFmac >= 0)
        p.steps[lastFmac].computeMac = 1u;
}

inline bool compileResolvedPath(const FastProg &prog, uint64_t branchMask, CompiledPath &outPath) // PERF-V0E
{
    outPath.branchMask = branchMask;
    outPath.branchCount = 0u;
    outPath.numSteps = 0u;
    outPath.numClips = 0u;
    outPath.commitQAtEnd = 0u;

    int32_t vi[16]{};
    bool viBackupValid = false;
    uint8_t viBackupReg = 0u;
    int32_t viBackupVal = 0;
    auto readVi = [&](uint8_t r) -> int32_t
    {
        if (r == 0u) return 0;
        if (viBackupValid && viBackupReg == r) return viBackupVal;
        return vi[r];
    };

    LocalClipRing clipRing;
    uint64_t cycle = 0ull;
    uint64_t vfReady[32][4]{};
    uint64_t viReady[16]{};
    uint64_t accReady[4]{};
    bool fdivValid = false;
    uint64_t fdivReadyCycle = 0ull;
    bool branchPending = false;
    uint32_t branchTargetPc = 0u;
    uint32_t branchDelay = 0u;
    bool ebitPending = false;

    uint32_t pc = prog.resolved[0].pc;
    while (true)
    {
        if (outPath.numSteps >= kMaxResolvedPairs)
            return false;
        const uint16_t idx = prog.slotToIdx[(pc >> 3) & (kNumSlots - 1u)];
        if (idx == 0xFFFFu)
            return false;
        const ResolvedPair &pair = prog.resolved[idx];
        const DecodedOp &up = pair.upper;
        const DecodedLower &lo = pair.lower;

        uint64_t readyCycle = cycle;
        if (up.kind != OP_NOP)
        {
            if (up.kind == OP_CLIP)
            {
                for (uint32_t c = 0; c < 3u; ++c)
                    readyCycle = std::max(readyCycle, vfReady[up.fs][c]);
                readyCycle = std::max(readyCycle, vfReady[up.ft][3]);
            }
            else
            {
                const bool hasAcc = (up.kind == OP_MADD || up.kind == OP_MSUB);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if ((up.dest & (1u << (3u - c))) != 0u)
                    {
                        readyCycle = std::max(readyCycle, vfReady[up.fs][c]);
                        if (up.src == SRC_VT_VEC)
                            readyCycle = std::max(readyCycle, vfReady[up.ft][c]);
                        if (hasAcc)
                            readyCycle = std::max(readyCycle, accReady[c]);
                    }
                }
                if (up.src == SRC_VT_BC && up.dest != 0u)
                    readyCycle = std::max(readyCycle, vfReady[up.ft][up.bc]);
            }
        }
        switch (lo.kind)
        {
        case LOP_IADDIU:
        case LOP_JR:
        case LOP_JALR:
            if (lo.r1 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r1]);
            break;
        case LOP_IAND:
        case LOP_IOR:
        case LOP_IBEQ:
        case LOP_IBNE:
            if (lo.r1 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r1]);
            if (lo.r2 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r2]);
            break;
        case LOP_DIV:
        case LOP_RSQRT:
            readyCycle = std::max(readyCycle, vfReady[lo.r1][lo.r3]);
            readyCycle = std::max(readyCycle, vfReady[lo.r2][lo.r4]);
            if (fdivValid) readyCycle = std::max(readyCycle, fdivReadyCycle);
            break;
        case LOP_WAITQ:
            if (fdivValid) readyCycle = std::max(readyCycle, fdivReadyCycle);
            break;
        default:
            break;
        }

        CompiledStep &st = outPath.steps[outPath.numSteps++];
        st = {};
        st.upper = up;
        st.lower = lo;
        st.pc = pair.pc;
        if (readyCycle > cycle)
        {
            cycle = readyCycle;
            if (fdivValid && fdivReadyCycle <= cycle)
            {
                st.commitQBefore = 1u;
                fdivValid = false;
            }
        }
        if (up.kind != OP_NOP && up.kind != OP_CLIP && !up.toAcc && up.fd != 0u &&
            (lo.kind == LOP_DIV || lo.kind == LOP_RSQRT) &&
            (lo.r1 == up.fd || lo.r2 == up.fd))
        {
            st.shadowReg = up.fd;
        }

        if (up.kind == OP_CLIP)
        {
            st.clipIdx = ++outPath.numClips;
            clipRing.push(cycle, st.clipIdx);
        }
        else if (up.kind != OP_NOP)
        {
            if (up.toAcc)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((up.dest & (1u << (3u - c))) != 0u)
                        accReady[c] = cycle + 1u;
            }
            else if (up.fd != 0u)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((up.dest & (1u << (3u - c))) != 0u)
                        vfReady[up.fd][c] = cycle + 4u;
            }
        }

        bool nextBackupValid = false;
        uint8_t nextBackupReg = 0u;
        int32_t nextBackupVal = 0;
        switch (lo.kind)
        {
        case LOP_IADDIU:
            if (lo.r2 != 0u)
            {
                nextBackupValid = true;
                nextBackupReg = lo.r2;
                nextBackupVal = vi[lo.r2];
                vi[lo.r2] = static_cast<int16_t>(vi[lo.r1] + lo.imm);
                viReady[lo.r2] = cycle + 1u;
            }
            break;
        case LOP_IAND:
            if (lo.r3 != 0u)
            {
                nextBackupValid = true;
                nextBackupReg = lo.r3;
                nextBackupVal = vi[lo.r3];
                vi[lo.r3] = vi[lo.r1] & vi[lo.r2];
                viReady[lo.r3] = cycle + 1u;
            }
            break;
        case LOP_IOR:
            if (lo.r3 != 0u)
            {
                nextBackupValid = true;
                nextBackupReg = lo.r3;
                nextBackupVal = vi[lo.r3];
                vi[lo.r3] = vi[lo.r1] | vi[lo.r2];
                viReady[lo.r3] = cycle + 1u;
            }
            break;
        case LOP_FCGET:
            if (lo.r2 != 0u)
            {
                st.clipIdx = static_cast<uint8_t>(clipRing.visible(cycle));
                viReady[lo.r2] = cycle + 1u;
            }
            break;
        case LOP_DIV:
            fdivReadyCycle = cycle + 7u;
            fdivValid = true;
            break;
        case LOP_RSQRT:
            fdivReadyCycle = cycle + 13u;
            fdivValid = true;
            break;
        case LOP_JR:
            branchPending = true;
            branchTargetPc = (static_cast<uint32_t>(static_cast<uint16_t>(readVi(lo.r1))) * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            branchDelay = 1u;
            break;
        case LOP_JALR:
            branchTargetPc = (static_cast<uint32_t>(static_cast<uint16_t>(readVi(lo.r1))) * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            if (lo.r2 != 0u)
            {
                vi[lo.r2] = static_cast<int32_t>((pc + 16u) / 8u);
                viReady[lo.r2] = cycle + 1u;
            }
            branchPending = true;
            branchDelay = 1u;
            break;
        case LOP_IBEQ:
        case LOP_IBNE:
        {
            const bool taken = ((branchMask >> outPath.branchCount) & 1ull) != 0ull;
            ++outPath.branchCount;
            if (taken)
            {
                branchPending = true;
                branchTargetPc = static_cast<uint32_t>(lo.targetSlot) * 8u;
                branchDelay = 1u;
            }
            break;
        }
        default:
            break;
        }

        viBackupValid = nextBackupValid;
        viBackupReg = nextBackupReg;
        viBackupVal = nextBackupVal;

        pc = (pc + 8u) & (PS2_VU0_CODE_SIZE - 1u);
        if (branchPending)
        {
            if (branchDelay == 0u)
            {
                pc = branchTargetPc;
                branchPending = false;
            }
            else
            {
                --branchDelay;
            }
        }

        bool programEnded = false;
        if (ebitPending)
            programEnded = true;
        else if (pair.eBit != 0u)
            ebitPending = true;

        ++cycle;
        if (fdivValid && fdivReadyCycle <= cycle)
        {
            st.commitQAfter = 1u;
            fdivValid = false;
        }
        if (programEnded)
        {
            outPath.endPC = static_cast<uint16_t>(pc);
            outPath.commitQAtEnd = fdivValid ? 1u : 0u;
            finalizeCompiledPath(outPath);
            return true;
        }
    }
}

// PERF-V0D: static DAG classifier for programs with branches, JALR/JR, CLIP/FCGET, DIV/RSQRT/WAITQ
inline bool classifyResolved(const uint8_t *vu0Code, uint32_t startPC, FastProg &outProg)
{
    outProg.isStraightFmac = false;
    outProg.numResolvedPairs = 0u;
    std::memset(outProg.slotToIdx, 0xFF, sizeof(outProg.slotToIdx));

    uint8_t visitState[kNumSlots]{}; // 0=unvisited, 1=visiting, 2=done(no-JR)
    int32_t viConst[16]{};
    uint16_t viKnownMask = 1u; // vi0 is always 0

    auto addPair = [&](uint32_t pc, ResolvedPair &outPair) -> bool
    {
        const uint32_t slot = (pc >> 3) & (kNumSlots - 1u);
        if (outProg.slotToIdx[slot] != 0xFFFFu)
        {
            outPair = outProg.resolved[outProg.slotToIdx[slot]];
            return true;
        }
        if (outProg.numResolvedPairs >= kMaxResolvedPairs)
            return false;

        uint32_t lower = 0u;
        uint32_t upper = 0u;
        std::memcpy(&lower, vu0Code + pc, 4);
        std::memcpy(&upper, vu0Code + pc + 4u, 4);
        if ((upper & 0xB8000000u) != 0u)
            return false;
        if (!decodeSupportedUpperResolved(upper, outPair.upper))
            return false;
        if (!decodeSupportedLower(lower, outPair.lower))
            return false;
        outPair.pc = static_cast<uint16_t>(pc);
        outPair.eBit = ((upper & 0x40000000u) != 0u) ? 1u : 0u;

        if (outPair.lower.kind == LOP_IBEQ || outPair.lower.kind == LOP_IBNE)
        {
            const uint32_t targetPC = (pc + 8u + static_cast<uint32_t>(static_cast<int32_t>(outPair.lower.imm) * 8)) & (PS2_VU0_CODE_SIZE - 1u);
            outPair.lower.targetSlot = static_cast<uint16_t>((targetPC >> 3) & (kNumSlots - 1u));
        }

        const uint16_t idx = outProg.numResolvedPairs++;
        outProg.slotToIdx[slot] = idx;
        outProg.resolved[idx] = outPair;
        return true;
    };

    auto applyLowerConst = [](const DecodedLower &lo, uint16_t &knownMask, int32_t c[16], uint8_t &delayReg)
    {
        delayReg = 0u;
        switch (lo.kind)
        {
        case LOP_IADDIU:
            if (lo.r2 != 0u)
            {
                if ((knownMask & (1u << lo.r1)) != 0u)
                {
                    c[lo.r2] = static_cast<int16_t>(c[lo.r1] + lo.imm);
                    knownMask |= static_cast<uint16_t>(1u << lo.r2); // PERF-V0E
                }
                else
                    knownMask &= static_cast<uint16_t>(~(1u << lo.r2));
                delayReg = lo.r2;
            }
            break;
        case LOP_IAND:
        case LOP_IOR:
            if (lo.r3 != 0u)
            {
                if ((knownMask & (1u << lo.r1)) != 0u && (knownMask & (1u << lo.r2)) != 0u)
                {
                    c[lo.r3] = (lo.kind == LOP_IAND) ? (c[lo.r1] & c[lo.r2]) : (c[lo.r1] | c[lo.r2]);
                    knownMask |= static_cast<uint16_t>(1u << lo.r3); // PERF-V0E
                }
                else
                    knownMask &= static_cast<uint16_t>(~(1u << lo.r3));
                delayReg = lo.r3;
            }
            break;
        case LOP_FCGET:
            if (lo.r2 != 0u)
                knownMask &= static_cast<uint16_t>(~(1u << lo.r2));
            break;
        default:
            break;
        }
    };

    struct ExploreFn
    {
        const uint8_t *vu0Code;
        FastProg &outProg;
        uint8_t *visitState;
        decltype(addPair) &addPairFn;
        decltype(applyLowerConst) &applyLowerFn;

        bool run(uint32_t pc, uint16_t knownMask, const int32_t cIn[16], uint8_t delayReg, bool &outHasJr)
        {
            const uint32_t slot = (pc >> 3) & (kNumSlots - 1u);
            if (visitState[slot] == 1u)
                return false;
            if (visitState[slot] == 2u)
                return true;

            visitState[slot] = 1u;
            int32_t c[16];
            std::memcpy(c, cIn, sizeof(c));

            ResolvedPair pair{};
            if (!addPairFn(pc, pair))
                return false;

            if (pair.eBit != 0u)
            {
                if (isLowerBranch(pair.lower.kind))
                    return false;
                const uint32_t delayPC = (pc + 8u) & (PS2_VU0_CODE_SIZE - 1u);
                ResolvedPair delayPair{};
                if (!addPairFn(delayPC, delayPair))
                    return false;
                if (delayPair.eBit != 0u || isLowerBranch(delayPair.lower.kind))
                    return false;
                visitState[slot] = 2u;
                return true;
            }

            if (isLowerBranch(pair.lower.kind))
            {
                const uint32_t delayPC = (pc + 8u) & (PS2_VU0_CODE_SIZE - 1u);
                const uint32_t delaySlot = (delayPC >> 3) & (kNumSlots - 1u);
                if (visitState[delaySlot] == 1u)
                    return false;
                ResolvedPair delayPair{};
                if (!addPairFn(delayPC, delayPair))
                    return false;
                if (delayPair.eBit != 0u || isLowerBranch(delayPair.lower.kind))
                    return false;

                if (pair.lower.kind == LOP_JR || pair.lower.kind == LOP_JALR)
                {
                    if ((knownMask & (1u << pair.lower.r1)) == 0u || delayReg == pair.lower.r1)
                        return false;
                    const uint32_t targetPC = (static_cast<uint32_t>(static_cast<uint16_t>(c[pair.lower.r1])) * 8u) & (PS2_VU0_CODE_SIZE - 1u);
                    if (pair.lower.kind == LOP_JALR && pair.lower.r2 != 0u)
                    {
                        c[pair.lower.r2] = static_cast<int32_t>((pc + 16u) / 8u);
                        knownMask |= static_cast<uint16_t>(1u << pair.lower.r2);
                    }
                    uint8_t nextDelay = 0u;
                    applyLowerFn(delayPair.lower, knownMask, c, nextDelay);
                    bool childHasJr = false;
                    if (!run(targetPC, knownMask, c, nextDelay, childHasJr))
                        return false;
                    if (pair.lower.kind == LOP_JR)
                    {
                        outHasJr = true;
                        visitState[slot] = 0u;
                    }
                    else
                    {
                        visitState[slot] = childHasJr ? 0u : 2u;
                        outHasJr = outHasJr || childHasJr;
                    }
                    return true;
                }
                else // IBEQ / IBNE
                {
                    uint8_t nextDelay = 0u;
                    applyLowerFn(delayPair.lower, knownMask, c, nextDelay);
                    const uint32_t ftPC = (pc + 16u) & (PS2_VU0_CODE_SIZE - 1u);
                    const uint32_t tkPC = static_cast<uint32_t>(pair.lower.targetSlot) * 8u;
                    bool childHasJr = false;
                    if (!run(ftPC, knownMask, c, nextDelay, childHasJr))
                        return false;
                    if (!run(tkPC, knownMask, c, nextDelay, childHasJr))
                        return false;
                    outHasJr = outHasJr || childHasJr;
                    visitState[slot] = childHasJr ? 0u : 2u;
                    return true;
                }
            }

            applyLowerFn(pair.lower, knownMask, c, delayReg);
            const uint32_t nextPC = (pc + 8u) & (PS2_VU0_CODE_SIZE - 1u);
            bool childHasJr = false;
            if (!run(nextPC, knownMask, c, delayReg, childHasJr))
                return false;
            outHasJr = outHasJr || childHasJr;
            visitState[slot] = childHasJr ? 0u : 2u;
            return true;
        }
    } explorer{vu0Code, outProg, visitState, addPair, applyLowerConst};

    bool hasJr = false;
    if (!explorer.run(startPC, viKnownMask, viConst, 0u, hasJr))
        return false;

    // PERF-V0E: precompute register masks and default path
    uint32_t readMask = 0u;
    uint16_t readViMask = 0u; // PERF-V0F
    bool usesQ = false;
    bool usesI = false;
    for (uint16_t i = 0; i < outProg.numResolvedPairs; ++i)
    {
        const DecodedOp &up = outProg.resolved[i].upper;
        const DecodedLower &lo = outProg.resolved[i].lower;
        if (up.kind == OP_CLIP)
        {
            readMask |= (1u << up.fs) | (1u << up.ft);
        }
        else if (up.kind != OP_NOP && up.dest != 0u)
        {
            readMask |= (1u << up.fs);
            if (up.src == SRC_VT_BC || up.src == SRC_VT_VEC)
                readMask |= (1u << up.ft);
            else if (up.src == SRC_Q)
                usesQ = true;
            else if (up.src == SRC_I)
                usesI = true;
            if (!up.toAcc && up.dest != 0xFu && up.fd != 0u)
                readMask |= (1u << up.fd);
        }
        if (lo.kind == LOP_DIV || lo.kind == LOP_RSQRT)
            readMask |= (1u << lo.r1) | (1u << lo.r2);
        switch (lo.kind) // PERF-V0F
        {
        case LOP_IADDIU:
            readViMask |= static_cast<uint16_t>((1u << lo.r1) | (1u << lo.r2));
            break;
        case LOP_IAND:
        case LOP_IOR:
            readViMask |= static_cast<uint16_t>((1u << lo.r1) | (1u << lo.r2) | (1u << lo.r3));
            break;
        case LOP_IBEQ:
        case LOP_IBNE:
            readViMask |= static_cast<uint16_t>((1u << lo.r1) | (1u << lo.r2));
            break;
        case LOP_JR:
        case LOP_JALR:
            readViMask |= static_cast<uint16_t>(1u << lo.r1);
            break;
        default:
            break;
        }
    }
    outProg.readVfMask = readMask & ~1u;
    outProg.readViMask = static_cast<uint16_t>(readViMask & ~1u); // PERF-V0F
    outProg.usesQ = usesQ;
    outProg.usesI = usesI;
    outProg.numPaths = 0u;
    if (!compileResolvedPath(outProg, 0ull, outProg.paths[0]))
        return false;
    outProg.numPaths = 1u;
    return true;
}

inline bool classifyStraightFmac(const uint8_t *vu0Code, uint32_t startPC, FastProg &outProg)
{
    bool seenEBit = false;
    uint32_t activeOps = 0u;
    uint32_t readMask = 0u;
    uint32_t writeMask = 0u;
    bool usesQ = false;
    bool usesI = false;
    for (uint32_t i = 0; i < kMaxPairs; ++i)
    {
        const uint32_t pc = (startPC + i * 8u) & (PS2_VU0_CODE_SIZE - 1u);
        uint32_t lower = 0;
        uint32_t upper = 0;
        std::memcpy(&lower, vu0Code + pc, 4);
        std::memcpy(&upper, vu0Code + pc + 4u, 4);

        // Reject I/M/D/T bits (bits 31, 29, 28, 27)
        if ((upper & 0xB8000000u) != 0u)
            return false;
        if (!isLowerNoop(lower))
            return false;
        DecodedOp op{};
        if (!decodeSupportedUpper(upper, op))
            return false;

        if (op.kind != OP_NOP && op.dest != 0u) // PERF-V0E: compact active ops
        {
            outProg.ops[activeOps++] = op;
            if ((writeMask & (1u << op.fs)) == 0u)
                readMask |= (1u << op.fs);
            if ((op.src == SRC_VT_BC || op.src == SRC_VT_VEC) && (writeMask & (1u << op.ft)) == 0u)
                readMask |= (1u << op.ft);
            else if (op.src == SRC_Q)
                usesQ = true;
            else if (op.src == SRC_I)
                usesI = true;
            if (!op.toAcc && op.fd != 0u)
            {
                if (op.dest != 0xFu && (writeMask & (1u << op.fd)) == 0u)
                    readMask |= (1u << op.fd);
                writeMask |= (1u << op.fd);
            }
        }

        if (seenEBit)
        {
            const uint32_t totalPairs = i + 1u;
            outProg.numOps = static_cast<uint8_t>(activeOps);
            outProg.endPC = (startPC + totalPairs * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            outProg.isStraightFmac = true; // PERF-V0D
            outProg.readVfMask = readMask & ~1u;
            outProg.writeVfMask = writeMask & ~1u;
            outProg.usesQ = usesQ;
            outProg.usesI = usesI;
            return true;
        }
        if ((upper & 0x40000000u) != 0u)
            seenEBit = true;
    }
    return false;
}

inline bool classifyProgram(const uint8_t *vu0Code, uint32_t startPC, FastProg &outProg)
{
    if (classifyStraightFmac(vu0Code, startPC, outProg))
        return true;
    return classifyResolved(vu0Code, startPC, outProg); // PERF-V0D
}

inline ClassifierCache &getClassifierCache(const uint8_t *vu0Code, uint64_t generation) // PERF-V0G
{
    static thread_local ClassifierCache s_cache;
    if (!s_cache.initialized || s_cache.generation != generation || s_cache.codePtr != vu0Code)
    {
        const uint64_t h = fnv1a64(vu0Code, PS2_VU0_CODE_SIZE);
        s_cache.generation = generation;
        s_cache.codePtr = vu0Code;
        if (!s_cache.initialized || h != s_cache.imageHash)
        {
            s_cache.imageHash = h;
            s_cache.initialized = true;
            std::memset(s_cache.state, 0, sizeof(s_cache.state));
            s_cache.numProgs = 0;
        }
    }
    return s_cache;
}

inline uint64_t getCachedImageHash(const uint8_t *vu0Code, uint64_t generation) // PERF-V0G
{
    return getClassifierCache(vu0Code, generation).imageHash;
}

inline const FastProg *lookupOrClassify(const uint8_t *vu0Code, uint64_t generation, uint32_t startPC)
{
    ClassifierCache &s_cache = getClassifierCache(vu0Code, generation); // PERF-V0G

    const uint32_t slot = (startPC >> 3) & (kNumSlots - 1u);
    const uint8_t st = s_cache.state[slot];
    if (st == 2u)
        return &s_cache.progs[s_cache.progIdx[slot]];
    if (st == 1u)
        return nullptr;

    FastProg candidate{};
    if (!classifyProgram(vu0Code, startPC, candidate) || s_cache.numProgs >= kMaxCachedProgs)
    {
        s_cache.state[slot] = 1u;
        return nullptr;
    }

    const uint8_t idx = static_cast<uint8_t>(s_cache.numProgs++);
    s_cache.progs[idx] = candidate;
    s_cache.progIdx[slot] = idx;
    s_cache.state[slot] = 2u;
    return &s_cache.progs[idx];
}

// Mirrors VU1Interpreter::normalizeOperand
inline float normalizeOperand(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
        bits &= 0x80000000u;
    else if (exponent == 0xFFu)
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline __m128 normalizeOperand4(__m128 value) // PERF-V0E: SSE4.1 blendv
{
    const __m128i vi = _mm_castps_si128(value);
    const __m128i signMask = _mm_set1_epi32(static_cast<int32_t>(0x80000000u));
    const __m128i expMask = _mm_set1_epi32(0x7F800000);
    const __m128i maxMag = _mm_set1_epi32(0x7F7FFFFF);

    const __m128i expBits = _mm_and_si128(vi, expMask);
    const __m128i signBits = _mm_and_si128(vi, signMask);
    const __m128i isExpZero = _mm_cmpeq_epi32(expBits, _mm_setzero_si128());
    const __m128i isExpMax = _mm_cmpeq_epi32(expBits, expMask);

    __m128i res = _mm_blendv_epi8(vi, signBits, isExpZero);
    res = _mm_blendv_epi8(res, _mm_or_si128(signBits, maxMag), isExpMax);
    return _mm_castsi128_ps(res);
}

// Mirrors VU1Interpreter::normalizeFmacExactResult
inline uint8_t normalizeFmacExactResult(float &value, long double exactResult)
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

// PERF-V0D: mirrors VU1Interpreter::normalizeResult
inline float normalizeResultScalar(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (magnitude == 0u)
    {
        // keep signed zero
    }
    else if (exponent == 0u)
    {
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        bits = sign | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// PERF-V0F: double-precision AVX FMAC fast path, bit-identical to scalar long double reference under MXCSR RZ
static_assert(sizeof(long double) == sizeof(double), "VU0 fast path assumes MSVC long double == double"); // PERF-V0F
template <bool ComputeMac>
inline void execFmacFast(const DecodedOp &op,
                         __m128 vf[32],
                         __m128 &vacc,
                         bool &dirtyAcc,
                         uint32_t &stickyFlags, // PERF-V0F
                         uint32_t &mac,
                         uint32_t &status,
                         __m128 vq,
                         __m128 viReg)
{
    const uint32_t sseDestMask = kBitRev4[op.dest];
    const bool hasAcc = (op.kind == OP_MADD || op.kind == OP_MSUB);

    const __m128 va = normalizeOperand4(vf[op.fs]);
    __m128 vb;
    if (op.src == SRC_VT_BC)
    {
        const __m128 vt = vf[op.ft];
        __m128 bcast;
        switch (op.bc)
        {
        case 0u: bcast = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(0, 0, 0, 0)); break;
        case 1u: bcast = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(1, 1, 1, 1)); break;
        case 2u: bcast = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(2, 2, 2, 2)); break;
        default: bcast = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 3, 3, 3)); break;
        }
        vb = normalizeOperand4(bcast);
    }
    else if (op.src == SRC_Q)
    {
        vb = vq;
    }
    else if (op.src == SRC_I)
    {
        vb = viReg;
    }
    else
    {
        vb = normalizeOperand4(vf[op.ft]);
    }

    const __m128 vac = hasAcc ? normalizeOperand4(vacc) : _mm_setzero_ps();
    const __m256d da = _mm256_cvtps_pd(va);                                  // PERF-V0F
    const __m256d db = _mm256_cvtps_pd(vb);                                  // PERF-V0F
    const __m256d dac = hasAcc ? _mm256_cvtps_pd(vac) : _mm256_setzero_pd(); // PERF-V0F

    __m256d dprod = _mm256_setzero_pd();
    __m256d dex;
    switch (op.kind) // PERF-V0F: mul then add/sub in double (no FMA) to match scalar long double
    {
    case OP_MADD:
        dprod = _mm256_mul_pd(da, db);
        dex = _mm256_add_pd(dac, dprod);
        break;
    case OP_MSUB:
        dprod = _mm256_mul_pd(da, db);
        dex = _mm256_sub_pd(dac, dprod);
        break;
    case OP_ADD:
        dex = _mm256_add_pd(da, db);
        break;
    case OP_SUB:
        dex = _mm256_sub_pd(da, db);
        break;
    default: // OP_MUL
        dprod = _mm256_mul_pd(da, db);
        dex = dprod;
        break;
    }

    const __m256d absMaskD = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL)); // PERF-V0F
    const __m256d zeroD = _mm256_setzero_pd();
    const __m256d fltMinD = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
    const __m256d fltMaxD = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));

    const __m256d absEx = _mm256_and_pd(dex, absMaskD);
    const __m256d exIsZero = _mm256_cmp_pd(absEx, zeroD, _CMP_EQ_OQ);
    const __m256d exGeMin = _mm256_cmp_pd(absEx, fltMinD, _CMP_GE_OQ);
    const __m256d exLeMax = _mm256_cmp_pd(absEx, fltMaxD, _CMP_LE_OQ);
    const __m256d exSafe = _mm256_or_pd(exIsZero, _mm256_and_pd(exGeMin, exLeMax));

    __m256d prodIsZero = zeroD;
    __m256d safeD = exSafe;
    if (hasAcc)
    {
        const __m256d absProd = _mm256_and_pd(dprod, absMaskD);
        prodIsZero = _mm256_cmp_pd(absProd, zeroD, _CMP_EQ_OQ);
        const __m256d prodGeMin = _mm256_cmp_pd(absProd, fltMinD, _CMP_GE_OQ);
        const __m256d prodLeMax = _mm256_cmp_pd(absProd, fltMaxD, _CMP_LE_OQ);
        const __m256d prodSafe = _mm256_or_pd(prodIsZero, _mm256_and_pd(prodGeMin, prodLeMax));
        safeD = _mm256_and_pd(exSafe, prodSafe);
    }

    const uint32_t safeBits = static_cast<uint32_t>(_mm256_movemask_pd(safeD)); // PERF-V0F
    if ((safeBits & sseDestMask) == sseDestMask)
    {
        const __m128 vres = _mm256_cvtpd_ps(dex); // PERF-V0F: truncates toward zero under MXCSR RC=11b
        const uint32_t zeroSse = static_cast<uint32_t>(_mm256_movemask_pd(exIsZero)) & sseDestMask;
        const uint32_t signSse = static_cast<uint32_t>(_mm256_movemask_pd(dex)) & sseDestMask;
        if (hasAcc)
        {
            const uint32_t prodZeroSse = static_cast<uint32_t>(_mm256_movemask_pd(prodIsZero)) & sseDestMask;
            const uint32_t prodSignSse = static_cast<uint32_t>(_mm256_movemask_pd(dprod)) & sseDestMask;
            stickyFlags |= ((zeroSse | prodZeroSse) != 0u ? 1u : 0u) | ((signSse | prodSignSse) != 0u ? 2u : 0u);
        }
        else
        {
            stickyFlags |= (zeroSse != 0u ? 1u : 0u) | (signSse != 0u ? 2u : 0u);
        }

        if constexpr (ComputeMac)
        {
            mac = kBitRev4[zeroSse] | (static_cast<uint32_t>(kBitRev4[signSse]) << 4);
            const uint32_t current = ((zeroSse != 0u) ? 1u : 0u) | ((signSse != 0u) ? 2u : 0u);
            status = (status & 0xFF0u) | current;
        }

        if (op.toAcc)
        {
            if (sseDestMask == 0xFu)
                vacc = vres;
            else
            {
                const __m128 laneMask = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(kSseLaneMasks[sseDestMask])));
                vacc = _mm_blendv_ps(vacc, vres, laneMask);
            }
            dirtyAcc = true;
        }
        else if (op.fd != 0u)
        {
            if (sseDestMask == 0xFu)
                vf[op.fd] = vres;
            else
            {
                const __m128 laneMask = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(kSseLaneMasks[sseDestMask])));
                vf[op.fd] = _mm_blendv_ps(vf[op.fd], vres, laneMask);
            }
        }
        return;
    }

    // Exact scalar fallback when any enabled lane falls outside the safe window
    alignas(16) float vs[4];
    alignas(16) float vt[4];
    alignas(16) float acArr[4];
    _mm_store_ps(vs, vf[op.fs]);
    _mm_store_ps(vt, vf[op.ft]);
    if (hasAcc)
        _mm_store_ps(acArr, vacc);

    float bval = 0.0f;
    if (op.src == SRC_VT_BC)
        bval = normalizeOperand(vt[op.bc]);
    else if (op.src == SRC_Q)
        bval = _mm_cvtss_f32(vq);
    else if (op.src == SRC_I)
        bval = _mm_cvtss_f32(viReg);

    float result[4]{};
    uint8_t laneFlags[4]{};
    uint32_t extraSticky = 0u;

    for (uint32_t c = 0; c < 4u; ++c)
    {
        const uint8_t lane = static_cast<uint8_t>(1u << (3u - c));
        if ((op.dest & lane) == 0u)
            continue;
        const float a = normalizeOperand(vs[c]);
        const float b = (op.src == SRC_VT_VEC) ? normalizeOperand(vt[c]) : bval;
        const float ac = hasAcc ? normalizeOperand(acArr[c]) : 0.0f;
        float val;
        long double ex;
        switch (op.kind)
        {
        case OP_ADD:
            val = a + b;
            ex = static_cast<long double>(a) + static_cast<long double>(b);
            break;
        case OP_SUB:
            val = a - b;
            ex = static_cast<long double>(a) - static_cast<long double>(b);
            break;
        case OP_MADD:
            val = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac)));
            ex = static_cast<long double>(ac) + static_cast<long double>(a) * static_cast<long double>(b);
            break;
        case OP_MSUB:
            val = _mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac)));
            ex = static_cast<long double>(ac) - static_cast<long double>(a) * static_cast<long double>(b);
            break;
        default:
            val = a * b;
            ex = static_cast<long double>(a) * static_cast<long double>(b);
            break;
        }
        laneFlags[c] = normalizeFmacExactResult(val, ex);
        result[c] = val;
        if (hasAcc)
        {
            float pv = a * b;
            extraSticky |= normalizeFmacExactResult(pv, static_cast<long double>(a) * static_cast<long double>(b)) & 0xFu;
        }
    }

    uint32_t newMac = 0u;
    uint32_t newStatus = 0u;
    for (uint32_t c = 0; c < 4u; ++c)
    {
        const uint8_t lane = static_cast<uint8_t>(1u << (3u - c));
        if ((op.dest & lane) == 0u)
            continue;
        const uint32_t f = laneFlags[c];
        if constexpr (ComputeMac)
        {
            if ((f & 0x1u) != 0u) newMac |= lane;
            if ((f & 0x2u) != 0u) newMac |= static_cast<uint32_t>(lane) << 4;
            if ((f & 0x4u) != 0u) newMac |= static_cast<uint32_t>(lane) << 8;
            if ((f & 0x8u) != 0u) newMac |= static_cast<uint32_t>(lane) << 12;
        }
        newStatus |= f;
    }
    const uint32_t current = newStatus & 0xFu;
    stickyFlags |= (current | extraSticky); // PERF-V0F
    if constexpr (ComputeMac)
    {
        mac = newMac;
        status = (status & 0xFF0u) | current;
    }

    if (op.toAcc)
    {
        if (!hasAcc)
            _mm_store_ps(acArr, vacc);
        for (uint32_t c = 0; c < 4u; ++c)
            if ((op.dest & (1u << (3u - c))) != 0u)
                acArr[c] = result[c];
        vacc = _mm_load_ps(acArr);
        dirtyAcc = true;
    }
    else if (op.fd != 0u)
    {
        alignas(16) float dst[4];
        _mm_store_ps(dst, vf[op.fd]);
        for (uint32_t c = 0; c < 4u; ++c)
            if ((op.dest & (1u << (3u - c))) != 0u)
                dst[c] = result[c];
        vf[op.fd] = _mm_load_ps(dst);
    }
}

// PERF-V0E: CLIP helper
inline uint32_t execClipStep(__m128 fsVec, __m128 ftVec, uint32_t workingClip)
{
    alignas(16) float fsArr[4];
    alignas(16) float ftArr[4];
    _mm_store_ps(fsArr, fsVec);
    _mm_store_ps(ftArr, ftVec);

    uint32_t wBits = 0u;
    std::memcpy(&wBits, &ftArr[3], sizeof(wBits));
    const int32_t limit = (wBits & 0x7F800000u) != 0u
                              ? static_cast<int32_t>(wBits & 0x7FFFFFFFu)
                              : 0x007FFFFF;
    const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        bits ^= signMask;
        int32_t orderedBits = 0;
        std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
        return orderedBits > limit;
    };

    uint32_t flags = 0u;
    if (exceedsClipPlane(fsArr[0], 0x00000000u)) flags |= 0x01u;
    if (exceedsClipPlane(fsArr[0], 0x80000000u)) flags |= 0x02u;
    if (exceedsClipPlane(fsArr[1], 0x00000000u)) flags |= 0x04u;
    if (exceedsClipPlane(fsArr[1], 0x80000000u)) flags |= 0x08u;
    if (exceedsClipPlane(fsArr[2], 0x00000000u)) flags |= 0x10u;
    if (exceedsClipPlane(fsArr[2], 0x80000000u)) flags |= 0x20u;

    return ((workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
}

inline void execDivRsqrtStep(const DecodedLower &lo, __m128 sVec, __m128 tVec, float &fdivValue, uint32_t &fdivStatusDi) // PERF-V0E
{
    alignas(16) float sArr[4], tArr[4];
    _mm_store_ps(sArr, sVec);
    _mm_store_ps(tArr, tVec);
    const float num = normalizeOperand(sArr[lo.r3]);
    if (lo.kind == LOP_DIV)
    {
        const float den = normalizeOperand(tArr[lo.r4]);
        if (den == 0.0f)
        {
            fdivStatusDi = (num == 0.0f) ? 0x10u : 0x20u;
            fdivValue = (std::signbit(num) != std::signbit(den))
                            ? -std::numeric_limits<float>::max()
                            : std::numeric_limits<float>::max();
        }
        else
        {
            fdivStatusDi = 0u;
            fdivValue = num / den;
        }
    }
    else // LOP_RSQRT
    {
        const float radicand = normalizeOperand(tArr[lo.r4]);
        const float den = std::sqrt(std::fabs(radicand));
        fdivStatusDi = (radicand < 0.0f) ? 0x10u : 0u;
        if (den != 0.0f)
        {
            fdivValue = num / den;
        }
        else
        {
            fdivStatusDi = (num == 0.0f) ? 0x10u : 0x20u;
            fdivValue = std::signbit(num)
                            ? -std::numeric_limits<float>::max()
                            : std::numeric_limits<float>::max();
        }
    }
    fdivValue = normalizeResultScalar(fdivValue);
}

// PERF-V0E: cold-path simulator/compiler when a dynamic IBEQ/IBNE diverges from cached paths
inline void resumeAndCompileResolved(const FastProg &prog,
                                     const CompiledPath &prefixPath,
                                     uint16_t divergeStep,
                                     uint64_t takenMask,
                                     uint8_t branchIdx,
                                     __m128 vf[32],
                                     __m128 &vacc,
                                     bool &dirtyAcc,
                                     int32_t vi[16],
                                     int32_t prevViVal,
                                     float &q,
                                     __m128 &vq,
                                     __m128 viReg,
                                     uint32_t &stickyFlags, // PERF-V0F
                                     uint32_t &mac,
                                     uint32_t &status,
                                     uint32_t &workingClip,
                                     uint32_t clipHistory[kMaxResolvedPairs + 1u],
                                     float &fdivValue,
                                     uint32_t &fdivStatusDi,
                                     uint16_t &outEndPC,
                                     uint32_t &outWriteVfMask,
                                     uint16_t &outWriteViMask)
{
    CompiledPath newPath{};
    newPath.numSteps = 0u;
    newPath.numClips = 0u;

    LocalClipRing clipRing;
    uint64_t cycle = 0ull;
    uint64_t vfReady[32][4]{};
    uint64_t viReady[16]{};
    uint64_t accReady[4]{};
    bool fdivValid = false;
    uint64_t fdivReadyCycle = 0ull;
    bool branchPending = false;
    uint32_t branchTargetPc = 0u;
    uint32_t branchDelay = 0u;
    bool ebitPending = false;
    bool viBackupValid = false;
    uint8_t viBackupReg = 0u;
    int32_t viBackupVal = 0;

    int32_t simVi[16]{};
    auto readSimVi = [&](uint8_t r) -> int32_t
    {
        if (r == 0u) return 0;
        if (viBackupValid && viBackupReg == r) return viBackupVal;
        return simVi[r];
    };

    uint8_t replayBranch = 0u;
    uint32_t pc = prog.resolved[0].pc;

    // 1. Replay timing state for prefix steps 0..divergeStep
    for (uint16_t s = 0; s <= divergeStep; ++s)
    {
        const CompiledStep &pst = prefixPath.steps[s];
        newPath.steps[newPath.numSteps++] = pst;
        const DecodedOp &up = pst.upper;
        const DecodedLower &lo = pst.lower;

        uint64_t readyCycle = cycle;
        if (up.kind != OP_NOP)
        {
            if (up.kind == OP_CLIP)
            {
                for (uint32_t c = 0; c < 3u; ++c) readyCycle = std::max(readyCycle, vfReady[up.fs][c]);
                readyCycle = std::max(readyCycle, vfReady[up.ft][3]);
            }
            else
            {
                const bool hasAcc = (up.kind == OP_MADD || up.kind == OP_MSUB);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if ((up.dest & (1u << (3u - c))) != 0u)
                    {
                        readyCycle = std::max(readyCycle, vfReady[up.fs][c]);
                        if (up.src == SRC_VT_VEC) readyCycle = std::max(readyCycle, vfReady[up.ft][c]);
                        if (hasAcc) readyCycle = std::max(readyCycle, accReady[c]);
                    }
                }
                if (up.src == SRC_VT_BC && up.dest != 0u)
                    readyCycle = std::max(readyCycle, vfReady[up.ft][up.bc]);
            }
        }
        switch (lo.kind)
        {
        case LOP_IADDIU:
        case LOP_JR:
        case LOP_JALR:
            if (lo.r1 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r1]);
            break;
        case LOP_IAND:
        case LOP_IOR:
        case LOP_IBEQ:
        case LOP_IBNE:
            if (lo.r1 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r1]);
            if (lo.r2 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r2]);
            break;
        case LOP_DIV:
        case LOP_RSQRT:
            readyCycle = std::max(readyCycle, vfReady[lo.r1][lo.r3]);
            readyCycle = std::max(readyCycle, vfReady[lo.r2][lo.r4]);
            if (fdivValid) readyCycle = std::max(readyCycle, fdivReadyCycle);
            break;
        case LOP_WAITQ:
            if (fdivValid) readyCycle = std::max(readyCycle, fdivReadyCycle);
            break;
        default:
            break;
        }
        if (readyCycle > cycle)
        {
            cycle = readyCycle;
            if (fdivValid && fdivReadyCycle <= cycle) fdivValid = false;
        }

        if (up.kind == OP_CLIP)
            clipRing.push(cycle, ++newPath.numClips);
        else if (up.kind != OP_NOP)
        {
            if (up.toAcc)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((up.dest & (1u << (3u - c))) != 0u) accReady[c] = cycle + 1u;
            }
            else if (up.fd != 0u)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((up.dest & (1u << (3u - c))) != 0u) vfReady[up.fd][c] = cycle + 4u;
            }
        }

        bool nextBackupValid = false;
        uint8_t nextBackupReg = 0u;
        int32_t nextBackupVal = 0;
        switch (lo.kind)
        {
        case LOP_IADDIU:
            if (lo.r2 != 0u) { nextBackupValid = true; nextBackupReg = lo.r2; nextBackupVal = simVi[lo.r2]; simVi[lo.r2] = static_cast<int16_t>(simVi[lo.r1] + lo.imm); viReady[lo.r2] = cycle + 1u; }
            break;
        case LOP_IAND:
            if (lo.r3 != 0u) { nextBackupValid = true; nextBackupReg = lo.r3; nextBackupVal = simVi[lo.r3]; simVi[lo.r3] = simVi[lo.r1] & simVi[lo.r2]; viReady[lo.r3] = cycle + 1u; }
            break;
        case LOP_IOR:
            if (lo.r3 != 0u) { nextBackupValid = true; nextBackupReg = lo.r3; nextBackupVal = simVi[lo.r3]; simVi[lo.r3] = simVi[lo.r1] | simVi[lo.r2]; viReady[lo.r3] = cycle + 1u; }
            break;
        case LOP_FCGET:
            if (lo.r2 != 0u) viReady[lo.r2] = cycle + 1u;
            break;
        case LOP_DIV:
            fdivReadyCycle = cycle + 7u; fdivValid = true; break;
        case LOP_RSQRT:
            fdivReadyCycle = cycle + 13u; fdivValid = true; break;
        case LOP_JR:
            branchPending = true; branchTargetPc = (static_cast<uint32_t>(static_cast<uint16_t>(readSimVi(lo.r1))) * 8u) & (PS2_VU0_CODE_SIZE - 1u); branchDelay = 1u; break;
        case LOP_JALR:
            branchTargetPc = (static_cast<uint32_t>(static_cast<uint16_t>(readSimVi(lo.r1))) * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            if (lo.r2 != 0u) { simVi[lo.r2] = static_cast<int32_t>((pc + 16u) / 8u); viReady[lo.r2] = cycle + 1u; }
            branchPending = true; branchDelay = 1u; break;
        case LOP_IBEQ:
        case LOP_IBNE:
            if (((takenMask >> replayBranch) & 1ull) != 0ull)
            {
                branchPending = true;
                branchTargetPc = static_cast<uint32_t>(lo.targetSlot) * 8u;
                branchDelay = 1u;
            }
            ++replayBranch;
            break;
        default:
            break;
        }
        viBackupValid = nextBackupValid;
        viBackupReg = nextBackupReg;
        viBackupVal = nextBackupVal;
        pc = (pc + 8u) & (PS2_VU0_CODE_SIZE - 1u);
        if (branchPending)
        {
            if (branchDelay == 0u) { pc = branchTargetPc; branchPending = false; }
            else { --branchDelay; }
        }
        if (pst.pc != 0xFFFFu && prog.resolved[prog.slotToIdx[(pst.pc >> 3) & (kNumSlots - 1u)]].eBit != 0u)
            ebitPending = true;
        ++cycle;
        if (fdivValid && fdivReadyCycle <= cycle) fdivValid = false;
    }

    // Seed live vi backup state from step divergeStep (IBEQ/IBNE never sets backup)
    (void)prevViVal;
    viBackupValid = false;
    auto readLiveVi = [&](uint8_t r) -> int32_t
    {
        if (r == 0u) return 0;
        if (viBackupValid && viBackupReg == r) return viBackupVal;
        return vi[r];
    };

    // 2. Execute + record remaining steps
    while (true)
    {
        const uint16_t idx = prog.slotToIdx[(pc >> 3) & (kNumSlots - 1u)];
        const ResolvedPair &pair = prog.resolved[idx];
        const DecodedOp &up = pair.upper;
        const DecodedLower &lo = pair.lower;

        uint64_t readyCycle = cycle;
        if (up.kind != OP_NOP)
        {
            if (up.kind == OP_CLIP)
            {
                for (uint32_t c = 0; c < 3u; ++c) readyCycle = std::max(readyCycle, vfReady[up.fs][c]);
                readyCycle = std::max(readyCycle, vfReady[up.ft][3]);
            }
            else
            {
                const bool hasAcc = (up.kind == OP_MADD || up.kind == OP_MSUB);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if ((up.dest & (1u << (3u - c))) != 0u)
                    {
                        readyCycle = std::max(readyCycle, vfReady[up.fs][c]);
                        if (up.src == SRC_VT_VEC) readyCycle = std::max(readyCycle, vfReady[up.ft][c]);
                        if (hasAcc) readyCycle = std::max(readyCycle, accReady[c]);
                    }
                }
                if (up.src == SRC_VT_BC && up.dest != 0u)
                    readyCycle = std::max(readyCycle, vfReady[up.ft][up.bc]);
            }
        }
        switch (lo.kind)
        {
        case LOP_IADDIU:
        case LOP_JR:
        case LOP_JALR:
            if (lo.r1 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r1]);
            break;
        case LOP_IAND:
        case LOP_IOR:
        case LOP_IBEQ:
        case LOP_IBNE:
            if (lo.r1 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r1]);
            if (lo.r2 != 0u) readyCycle = std::max(readyCycle, viReady[lo.r2]);
            break;
        case LOP_DIV:
        case LOP_RSQRT:
            readyCycle = std::max(readyCycle, vfReady[lo.r1][lo.r3]);
            readyCycle = std::max(readyCycle, vfReady[lo.r2][lo.r4]);
            if (fdivValid) readyCycle = std::max(readyCycle, fdivReadyCycle);
            break;
        case LOP_WAITQ:
            if (fdivValid) readyCycle = std::max(readyCycle, fdivReadyCycle);
            break;
        default:
            break;
        }

        CompiledStep &st = newPath.steps[newPath.numSteps++];
        st = {};
        st.upper = up;
        st.lower = lo;
        st.pc = pair.pc;
        if (readyCycle > cycle)
        {
            cycle = readyCycle;
            if (fdivValid && fdivReadyCycle <= cycle)
            {
                st.commitQBefore = 1u;
                q = fdivValue;
                vq = _mm_set1_ps(normalizeOperand(q));
                status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
                fdivValid = false;
            }
        }

        uint8_t shadowReg = 0u;
        __m128 shadowOldVf{};
        __m128 shadowNewVf{};
        if (up.kind != OP_NOP && up.kind != OP_CLIP && !up.toAcc && up.fd != 0u &&
            (lo.kind == LOP_DIV || lo.kind == LOP_RSQRT) &&
            (lo.r1 == up.fd || lo.r2 == up.fd))
        {
            shadowReg = up.fd;
            st.shadowReg = shadowReg;
            shadowOldVf = vf[shadowReg];
        }

        if (up.kind == OP_CLIP)
        {
            workingClip = execClipStep(vf[up.fs], vf[up.ft], workingClip);
            st.clipIdx = ++newPath.numClips;
            clipHistory[st.clipIdx] = workingClip;
            clipRing.push(cycle, st.clipIdx);
        }
        else if (up.kind != OP_NOP)
        {
            if (up.dest != 0u)
                execFmacFast<true>(up, vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, viReg); // PERF-V0F
            if (up.toAcc)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((up.dest & (1u << (3u - c))) != 0u) accReady[c] = cycle + 1u;
            }
            else if (up.fd != 0u)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((up.dest & (1u << (3u - c))) != 0u) vfReady[up.fd][c] = cycle + 4u;
            }
        }

        if (shadowReg != 0u)
        {
            shadowNewVf = vf[shadowReg];
            vf[shadowReg] = shadowOldVf;
        }

        bool nextBackupValid = false;
        uint8_t nextBackupReg = 0u;
        int32_t nextBackupVal = 0;
        switch (lo.kind)
        {
        case LOP_IADDIU:
            if (lo.r2 != 0u)
            {
                nextBackupValid = true; nextBackupReg = lo.r2; nextBackupVal = vi[lo.r2];
                vi[lo.r2] = static_cast<int16_t>(vi[lo.r1] + lo.imm);
                viReady[lo.r2] = cycle + 1u;
            }
            break;
        case LOP_IAND:
            if (lo.r3 != 0u)
            {
                nextBackupValid = true; nextBackupReg = lo.r3; nextBackupVal = vi[lo.r3];
                vi[lo.r3] = vi[lo.r1] & vi[lo.r2];
                viReady[lo.r3] = cycle + 1u;
            }
            break;
        case LOP_IOR:
            if (lo.r3 != 0u)
            {
                nextBackupValid = true; nextBackupReg = lo.r3; nextBackupVal = vi[lo.r3];
                vi[lo.r3] = vi[lo.r1] | vi[lo.r2];
                viReady[lo.r3] = cycle + 1u;
            }
            break;
        case LOP_FCGET:
            if (lo.r2 != 0u)
            {
                st.clipIdx = static_cast<uint8_t>(clipRing.visible(cycle));
                vi[lo.r2] = static_cast<int32_t>(clipHistory[st.clipIdx] & 0x0FFFu);
                viReady[lo.r2] = cycle + 1u;
            }
            break;
        case LOP_DIV:
            execDivRsqrtStep(lo, vf[lo.r1], vf[lo.r2], fdivValue, fdivStatusDi);
            fdivReadyCycle = cycle + 7u; fdivValid = true; break;
        case LOP_RSQRT:
            execDivRsqrtStep(lo, vf[lo.r1], vf[lo.r2], fdivValue, fdivStatusDi);
            fdivReadyCycle = cycle + 13u; fdivValid = true; break;
        case LOP_JR:
            branchPending = true;
            branchTargetPc = (static_cast<uint32_t>(static_cast<uint16_t>(readLiveVi(lo.r1))) * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            branchDelay = 1u;
            break;
        case LOP_JALR:
            branchTargetPc = (static_cast<uint32_t>(static_cast<uint16_t>(readLiveVi(lo.r1))) * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            if (lo.r2 != 0u) { vi[lo.r2] = static_cast<int32_t>((pc + 16u) / 8u); viReady[lo.r2] = cycle + 1u; }
            branchPending = true; branchDelay = 1u; break;
        case LOP_IBEQ:
        case LOP_IBNE:
        {
            const int16_t v1 = static_cast<int16_t>(readLiveVi(lo.r1));
            const int16_t v2 = static_cast<int16_t>(readLiveVi(lo.r2));
            const bool taken = (lo.kind == LOP_IBEQ) ? (v1 == v2) : (v1 != v2);
            if (taken)
            {
                takenMask |= (1ull << branchIdx);
                branchPending = true;
                branchTargetPc = static_cast<uint32_t>(lo.targetSlot) * 8u;
                branchDelay = 1u;
            }
            ++branchIdx;
            break;
        }
        default:
            break;
        }

        if (shadowReg != 0u)
            vf[shadowReg] = shadowNewVf;

        viBackupValid = nextBackupValid;
        viBackupReg = nextBackupReg;
        viBackupVal = nextBackupVal;

        pc = (pc + 8u) & (PS2_VU0_CODE_SIZE - 1u);
        if (branchPending)
        {
            if (branchDelay == 0u) { pc = branchTargetPc; branchPending = false; }
            else { --branchDelay; }
        }

        bool programEnded = false;
        if (ebitPending) programEnded = true;
        else if (pair.eBit != 0u) ebitPending = true;

        ++cycle;
        if (fdivValid && fdivReadyCycle <= cycle)
        {
            st.commitQAfter = 1u;
            q = fdivValue;
            vq = _mm_set1_ps(normalizeOperand(q));
            status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
            fdivValid = false;
        }

        if (programEnded)
        {
            if (fdivValid)
            {
                newPath.commitQAtEnd = 1u;
                q = fdivValue;
                status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
            }
            newPath.endPC = static_cast<uint16_t>(pc);
            newPath.branchMask = takenMask;
            newPath.branchCount = branchIdx;
            finalizeCompiledPath(newPath);
            if (prog.numPaths < kMaxCachedPaths)
                prog.paths[prog.numPaths++] = newPath;
            outEndPC = newPath.endPC;
            outWriteVfMask = newPath.writeVfMask;
            outWriteViMask = newPath.writeViMask;
            return;
        }
    }
}

// PERF-V0E: precompiled-path executor for programs with branches, JALR/JR, CLIP/FCGET, DIV/RSQRT/WAITQ
inline void executeResolved(const FastProg &prog, R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u); // RC = 11b: toward zero (mirrors VU1Interpreter::run)

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    for (uint32_t m = prog.readVfMask >> 1, idx = 1u; m != 0u; m >>= 1, ++idx)
    {
        if ((m & 1u) != 0u)
            vf[idx] = ctx->vu0_vf[idx];
    }
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    int32_t vi[16];
    vi[0] = 0;
    for (uint32_t m = prog.readViMask >> 1, i = 1u; m != 0u; m >>= 1, ++i) // PERF-V0F
    {
        if ((m & 1u) != 0u)
            vi[i] = static_cast<int16_t>(ctx->vi[i]);
    }

    float q = ctx->vu0_q;
    __m128 vq = prog.usesQ ? _mm_set1_ps(normalizeOperand(q)) : _mm_setzero_ps();
    const __m128 viReg = prog.usesI ? _mm_set1_ps(normalizeOperand(ctx->vu0_i)) : _mm_setzero_ps();
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
    uint32_t workingClip = ctx->vu0_clip_flags & 0xFFFFFFu;
    uint32_t clipHistory[kMaxResolvedPairs + 1u];
    clipHistory[0] = workingClip;

    uint32_t stickyFlags = 0u; // PERF-V0F

    float fdivValue = 0.0f;
    uint32_t fdivStatusDi = 0u;
    int32_t prevViVal = 0;

    const CompiledPath *curPath = &prog.paths[0];
    uint64_t takenMask = 0ull;
    uint8_t branchIdx = 0u;
    bool finishedViaResume = false;
    uint16_t endPC = curPath->endPC;
    uint32_t writeVfMask = curPath->writeVfMask;
    uint16_t writeViMask = curPath->writeViMask;

    for (uint16_t s = 0; s < curPath->numSteps; ++s)
    {
        const CompiledStep &st = curPath->steps[s];
        if (st.commitQBefore != 0u)
        {
            q = fdivValue;
            if (prog.usesQ) vq = _mm_set1_ps(normalizeOperand(q));
            status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
        }

        __m128 shadowOldVf{};
        __m128 shadowNewVf{};
        if (st.shadowReg != 0u)
            shadowOldVf = vf[st.shadowReg];

        const DecodedOp &up = st.upper;
        if (up.kind == OP_CLIP)
        {
            workingClip = execClipStep(vf[up.fs], vf[up.ft], workingClip);
            clipHistory[st.clipIdx] = workingClip;
        }
        else if (up.kind != OP_NOP && up.dest != 0u)
        {
            if (st.computeMac != 0u)
                execFmacFast<true>(up, vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, viReg); // PERF-V0F
            else
                execFmacFast<false>(up, vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, viReg); // PERF-V0F
        }

        if (st.shadowReg != 0u)
        {
            shadowNewVf = vf[st.shadowReg];
            vf[st.shadowReg] = shadowOldVf;
        }

        const DecodedLower &lo = st.lower;
        switch (lo.kind)
        {
        case LOP_NOP:
        case LOP_WAITQ:
        case LOP_JR:
            break;
        case LOP_JALR:
            if (lo.r2 != 0u)
                vi[lo.r2] = static_cast<int32_t>((st.pc + 16u) / 8u);
            break;
        case LOP_IADDIU:
            if (st.saveBackup != 0u) prevViVal = vi[lo.r2];
            if (lo.r2 != 0u) vi[lo.r2] = static_cast<int16_t>(vi[lo.r1] + lo.imm);
            break;
        case LOP_IAND:
            if (st.saveBackup != 0u) prevViVal = vi[lo.r3];
            if (lo.r3 != 0u) vi[lo.r3] = vi[lo.r1] & vi[lo.r2];
            break;
        case LOP_IOR:
            if (st.saveBackup != 0u) prevViVal = vi[lo.r3];
            if (lo.r3 != 0u) vi[lo.r3] = vi[lo.r1] | vi[lo.r2];
            break;
        case LOP_FCGET:
            if (lo.r2 != 0u)
                vi[lo.r2] = static_cast<int32_t>(clipHistory[st.clipIdx] & 0x0FFFu);
            break;
        case LOP_DIV:
        case LOP_RSQRT:
            execDivRsqrtStep(lo, vf[lo.r1], vf[lo.r2], fdivValue, fdivStatusDi);
            break;
        case LOP_IBEQ:
        case LOP_IBNE:
        {
            const int16_t v1 = static_cast<int16_t>((st.backupMask & 1u) ? prevViVal : vi[lo.r1]);
            const int16_t v2 = static_cast<int16_t>((st.backupMask & 2u) ? prevViVal : vi[lo.r2]);
            const bool taken = (lo.kind == LOP_IBEQ) ? (v1 == v2) : (v1 != v2);
            if (taken)
                takenMask |= (1ull << branchIdx);
            const bool expectedTaken = ((curPath->branchMask >> branchIdx) & 1ull) != 0ull;
            ++branchIdx;
            if (taken != expectedTaken)
            {
                const uint64_t prefixMatchMask = (branchIdx >= 64u) ? ~0ull : ((1ull << branchIdx) - 1ull);
                const CompiledPath *matched = nullptr;
                for (uint8_t p = 0; p < prog.numPaths; ++p)
                {
                    if (prog.paths[p].branchCount >= branchIdx &&
                        (prog.paths[p].branchMask & prefixMatchMask) == takenMask)
                    {
                        matched = &prog.paths[p];
                        break;
                    }
                }
                if (matched)
                {
                    curPath = matched;
                    endPC = curPath->endPC;
                    writeVfMask = curPath->writeVfMask;
                    writeViMask = curPath->writeViMask;
                }
                else
                {
                    if (st.shadowReg != 0u)
                        vf[st.shadowReg] = shadowNewVf;
                    resumeAndCompileResolved(
                        prog, *curPath, s, takenMask, branchIdx,
                        vf, vacc, dirtyAcc, vi, prevViVal,
                        q, vq, viReg, stickyFlags, // PERF-V0F
                        mac, status, workingClip, clipHistory, fdivValue, fdivStatusDi,
                        endPC, writeVfMask, writeViMask);
                    finishedViaResume = true;
                }
            }
            break;
        }
        default:
            break;
        }

        if (finishedViaResume)
            break;

        if (st.shadowReg != 0u)
            vf[st.shadowReg] = shadowNewVf;

        if (st.commitQAfter != 0u)
        {
            q = fdivValue;
            if (prog.usesQ) vq = _mm_set1_ps(normalizeOperand(q));
            status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
        }
    }

    if (!finishedViaResume && curPath->commitQAtEnd != 0u)
    {
        q = fdivValue;
        status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
    }

    status |= (stickyFlags & 0xFu) << 6; // PERF-V0F

    ctx->vu0_vf[0] = vf[0];
    for (uint32_t m = writeVfMask >> 1, r = 1u; m != 0u; m >>= 1, ++r)
    {
        if ((m & 1u) != 0u)
            ctx->vu0_vf[r] = vf[r];
    }
    if (dirtyAcc)
        ctx->vu0_acc = vacc;
    for (uint32_t m = writeViMask >> 1, r = 1u; m != 0u; m >>= 1, ++r)
    {
        if ((m & 1u) != 0u)
            ctx->vi[r] = static_cast<uint16_t>(vi[r]);
    }
    ctx->vi[0] = 0;

    ctx->vu0_q = q;
    const uint32_t r0 = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r)));
    ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(0x3F800000u | (r0 & 0x007FFFFFu))));
    ctx->vu0_mac_flags = mac;
    ctx->vu0_status = static_cast<uint16_t>(status);
    ctx->vu0_clip_flags = workingClip;
    ctx->vu0_clip_flags2 = workingClip;
    ctx->vu0_pc = endPC;
    ctx->vu0_tpc = endPC;
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u);
    ctx->vu0_vpu_stat2 = 0u;

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

inline void executeFast(const FastProg &prog, R5900Context *ctx)
{
    if (!prog.isStraightFmac) // PERF-V0D
    {
        executeResolved(prog, ctx);
        return;
    }

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    for (uint32_t m = prog.readVfMask >> 1, i = 1u; m != 0u; m >>= 1, ++i)
    {
        if ((m & 1u) != 0u)
            vf[i] = ctx->vu0_vf[i];
    }
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    ctx->vi[0] = 0;

    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;

    if (prog.numOps > 0u)
    {
        const uint32_t prevCsr = _mm_getcsr(); // PERF-V0F
        const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
        if (changeCsr)
            _mm_setcsr(prevCsr | 0x6000u); // RC = 11b: toward zero (mirrors VU1Interpreter::run)

        const __m128 vq = prog.usesQ ? _mm_set1_ps(normalizeOperand(ctx->vu0_q)) : _mm_setzero_ps();
        const __m128 viReg = prog.usesI ? _mm_set1_ps(normalizeOperand(ctx->vu0_i)) : _mm_setzero_ps();
        uint32_t stickyFlags = 0u; // PERF-V0F

        const uint32_t last = prog.numOps - 1u;
        for (uint32_t i = 0; i < last; ++i)
            execFmacFast<false>(prog.ops[i], vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, viReg); // PERF-V0F
        execFmacFast<true>(prog.ops[last], vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, viReg);  // PERF-V0F

        status |= (stickyFlags & 0xFu) << 6; // PERF-V0F

        if (changeCsr) // PERF-V0F
            _mm_setcsr(prevCsr);
    }

    ctx->vu0_vf[0] = vf[0];
    for (uint32_t m = prog.writeVfMask >> 1, idx = 1u; m != 0u; m >>= 1, ++idx)
    {
        if ((m & 1u) != 0u)
            ctx->vu0_vf[idx] = vf[idx];
    }
    if (dirtyAcc)
        ctx->vu0_acc = vacc;

    const uint32_t r0 = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r)));
    ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(0x3F800000u | (r0 & 0x007FFFFFu))));
    ctx->vu0_mac_flags = mac;
    ctx->vu0_status = static_cast<uint16_t>(status);
    ctx->vu0_clip_flags2 = ctx->vu0_clip_flags;
    ctx->vu0_pc = prog.endPC;
    ctx->vu0_tpc = prog.endPC;
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u);
    ctx->vu0_vpu_stat2 = 0u;
}

struct MismatchRec
{
    uint32_t pc;
    char field[24];
    uint64_t fastVal;
    uint64_t normVal;
};

struct CheckStats
{
    std::mutex mtx;
    uint64_t calls = 0;
    uint64_t fast = 0;
    uint64_t mismatches = 0;
    uint32_t recorded = 0;
    MismatchRec first[5]{};
    std::chrono::steady_clock::time_point lastPrint{};
};

inline CheckStats &checkStats()
{
    static CheckStats s;
    return s;
}

inline void compareAndRecord(uint32_t startPC, const R5900Context &fastCtx, const R5900Context &normCtx)
{
    bool anyMismatch = false;
    char firstField[24]{};
    uint64_t firstFast = 0;
    uint64_t firstNorm = 0;

    auto cmpU64 = [&](const char *name, uint64_t fv, uint64_t nv)
    {
        if (fv != nv && !anyMismatch)
        {
            anyMismatch = true;
            std::snprintf(firstField, sizeof(firstField), "%s", name);
            firstFast = fv;
            firstNorm = nv;
        }
    };

    auto cmpM128 = [&](const char *prefix, int idx, __m128 fm, __m128 nm)
    {
        alignas(16) uint32_t fw[4]{}, nw[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fw), _mm_castps_si128(fm));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(nw), _mm_castps_si128(nm));
        static constexpr char kLanes[4] = {'x', 'y', 'z', 'w'};
        for (int l = 0; l < 4; ++l)
        {
            if (fw[l] != nw[l] && !anyMismatch)
            {
                anyMismatch = true;
                if (idx >= 0)
                    std::snprintf(firstField, sizeof(firstField), "%s%d.%c", prefix, idx, kLanes[l]);
                else
                    std::snprintf(firstField, sizeof(firstField), "%s.%c", prefix, kLanes[l]);
                firstFast = fw[l];
                firstNorm = nw[l];
            }
        }
    };

    auto cmpF32 = [&](const char *name, float ff, float nf)
    {
        uint32_t fb = 0, nb = 0;
        std::memcpy(&fb, &ff, 4);
        std::memcpy(&nb, &nf, 4);
        cmpU64(name, fb, nb);
    };

    for (int i = 0; i < 32; ++i)
        cmpM128("vf", i, fastCtx.vu0_vf[i], normCtx.vu0_vf[i]);
    for (int i = 0; i < 16; ++i)
    {
        char n[16];
        std::snprintf(n, sizeof(n), "vi%d", i);
        cmpU64(n, fastCtx.vi[i], normCtx.vi[i]);
    }
    cmpM128("acc", -1, fastCtx.vu0_acc, normCtx.vu0_acc);
    cmpF32("q", fastCtx.vu0_q, normCtx.vu0_q);
    cmpF32("p", fastCtx.vu0_p, normCtx.vu0_p);
    cmpF32("i", fastCtx.vu0_i, normCtx.vu0_i);
    cmpM128("r", -1, fastCtx.vu0_r, normCtx.vu0_r);
    cmpU64("mac", fastCtx.vu0_mac_flags, normCtx.vu0_mac_flags);
    cmpU64("status", fastCtx.vu0_status, normCtx.vu0_status);
    cmpU64("clip", fastCtx.vu0_clip_flags, normCtx.vu0_clip_flags);
    cmpU64("clip2", fastCtx.vu0_clip_flags2, normCtx.vu0_clip_flags2);
    cmpU64("pc", fastCtx.vu0_pc, normCtx.vu0_pc);
    cmpU64("tpc", fastCtx.vu0_tpc, normCtx.vu0_tpc);
    cmpU64("vpu_stat", fastCtx.vu0_vpu_stat, normCtx.vu0_vpu_stat);
    cmpU64("vpu_stat2", fastCtx.vu0_vpu_stat2, normCtx.vu0_vpu_stat2);
    cmpU64("itop", fastCtx.vu0_itop, normCtx.vu0_itop);

    CheckStats &cs = checkStats();
    std::lock_guard<std::mutex> lock(cs.mtx);
    cs.calls++;
    cs.fast++;
    if (anyMismatch)
    {
        cs.mismatches++;
        if (cs.recorded < 5u)
        {
            MismatchRec &rec = cs.first[cs.recorded++];
            rec.pc = startPC;
            std::snprintf(rec.field, sizeof(rec.field), "%s", firstField);
            rec.fastVal = firstFast;
            rec.normVal = firstNorm;
        }
    }
}

inline void maybePrintCheckStats(bool countNonFastCall)
{
    CheckStats &cs = checkStats();
    std::lock_guard<std::mutex> lock(cs.mtx);
    if (countNonFastCall)
        cs.calls++;
    const auto now = std::chrono::steady_clock::now();
    if (cs.lastPrint == std::chrono::steady_clock::time_point{})
    {
        cs.lastPrint = now;
        return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - cs.lastPrint).count() < 5)
        return;
    cs.lastPrint = now;
    std::printf("[v0fast] calls=%llu fast=%llu mismatches=%llu",
                static_cast<unsigned long long>(cs.calls),
                static_cast<unsigned long long>(cs.fast),
                static_cast<unsigned long long>(cs.mismatches));
    if (cs.recorded > 0u)
    {
        std::printf(" first:");
        for (uint32_t i = 0; i < cs.recorded; ++i)
        {
            std::printf(" pc=%03x field=%s(f=0x%llx,n=0x%llx)",
                        cs.first[i].pc,
                        cs.first[i].field,
                        static_cast<unsigned long long>(cs.first[i].fastVal),
                        static_cast<unsigned long long>(cs.first[i].normVal));
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}
} // namespace vu0fast
