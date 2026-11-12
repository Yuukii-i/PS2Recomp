// vu0_fast.inc -- Task V0B: fast path for straight-line VU0 FMAC microprograms
// (PS2X_VU0_FAST default ON, =0 disables;PS2X_VU0_FAST_CHECK=1 default OFF).
// Included by ps2_runtime.cpp before PS2Runtime::executeVU0Microprogram. No header edits.

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
constexpr uint32_t kNumSlots = PS2_VU0_CODE_SIZE / 8u; // 512
constexpr uint32_t kMaxCachedProgs = 64u;

enum OpKind : uint8_t
{
    OP_NOP = 0,
    OP_MUL = 1,
    OP_MADD = 2
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

struct FastProg
{
    uint32_t endPC;
    uint8_t numOps;
    DecodedOp ops[kMaxPairs];
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
        const char *p = std::getenv("PS2X_VU0_PCPROF");
        return (v && std::atoi(v) != 0) || (p && p[0] == '1');
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

inline bool classifyProgram(const uint8_t *vu0Code, uint32_t startPC, FastProg &outProg)
{
    bool seenEBit = false;
    uint32_t totalPairs = 0;
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
        if (!decodeSupportedUpper(upper, outProg.ops[i]))
            return false;

        if (seenEBit)
        {
            totalPairs = i + 1u;
            outProg.numOps = static_cast<uint8_t>(totalPairs);
            outProg.endPC = (startPC + totalPairs * 8u) & (PS2_VU0_CODE_SIZE - 1u);
            return true;
        }
        if ((upper & 0x40000000u) != 0u)
            seenEBit = true;
    }
    return false;
}

inline const FastProg *lookupOrClassify(const uint8_t *vu0Code, uint64_t generation, uint32_t startPC)
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

inline void executeFast(const FastProg &prog, R5900Context *ctx)
{
    ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    ctx->vi[0] = 0;

    float *const acc = reinterpret_cast<float *>(&ctx->vu0_acc);
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;

    for (uint32_t i = 0; i < prog.numOps; ++i)
    {
        const DecodedOp &op = prog.ops[i];
        if (op.kind == OP_NOP || op.dest == 0u)
            continue;

        const float *const vs = reinterpret_cast<const float *>(&ctx->vu0_vf[op.fs]);
        const float *const vt = reinterpret_cast<const float *>(&ctx->vu0_vf[op.ft]);
        float bval = 0.0f;
        if (op.src == SRC_VT_BC)
            bval = normalizeOperand(vt[op.bc]);
        else if (op.src == SRC_Q)
            bval = normalizeOperand(ctx->vu0_q);
        else if (op.src == SRC_I)
            bval = normalizeOperand(ctx->vu0_i);

        const bool madd = (op.kind == OP_MADD);
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
            const float ac = madd ? normalizeOperand(acc[c]) : 0.0f;
            float val;
            long double ex;
            if (madd)
            {
                val = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac)));
                ex = static_cast<long double>(ac) + static_cast<long double>(a) * static_cast<long double>(b);
            }
            else
            {
                val = a * b;
                ex = static_cast<long double>(a) * static_cast<long double>(b);
            }
            laneFlags[c] = normalizeFmacExactResult(val, ex);
            result[c] = val;
            if (madd)
            {
                float pv = a * b;
                extraSticky |= normalizeFmacExactResult(pv, static_cast<long double>(a) * static_cast<long double>(b)) & 0xFu;
            }
        }

        // Mirrors VU1Interpreter::updateFmacFlags (immediate-flag mode)
        uint32_t newMac = 0u;
        uint32_t newStatus = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint8_t lane = static_cast<uint8_t>(1u << (3u - c));
            if ((op.dest & lane) == 0u)
                continue;
            const uint32_t f = laneFlags[c];
            if ((f & 0x1u) != 0u) newMac |= lane;
            if ((f & 0x2u) != 0u) newMac |= static_cast<uint32_t>(lane) << 4;
            if ((f & 0x4u) != 0u) newMac |= static_cast<uint32_t>(lane) << 8;
            if ((f & 0x8u) != 0u) newMac |= static_cast<uint32_t>(lane) << 12;
            newStatus |= f;
        }
        const uint32_t current = newStatus & 0xFu;
        mac = newMac;
        status = (status & 0xFF0u) | current | ((current | extraSticky) << 6);

        if (op.toAcc)
        {
            for (uint32_t c = 0; c < 4u; ++c)
                if ((op.dest & (1u << (3u - c))) != 0u)
                    acc[c] = result[c];
        }
        else if (op.fd != 0u)
        {
            float *const dst = reinterpret_cast<float *>(&ctx->vu0_vf[op.fd]);
            for (uint32_t c = 0; c < 4u; ++c)
                if ((op.dest & (1u << (3u - c))) != 0u)
                    dst[c] = result[c];
        }
    }

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
