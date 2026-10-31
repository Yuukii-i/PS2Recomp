// vu1_hot_ops.h -- inline, constant-folding op implementations for the generated VU1 hot blocks (generated/vu1_hot_blocks.cpp).
// Exact re-implementations of the interpreter semantics (fastUpper in ps2_vu1_core.cpp, ps2_vu1_upper.cpp, ps2_vu1_lower.cpp):
// the FMAC family runs the same float/double math on 4 lanes at once with AVX2 and derives the MAC/STATUS bits from the compare masks.
// The including TU must have done `#define private public` + #include "runtime/ps2_vu1.h" and "../vu1_native_api.h".
#pragma once

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <limits>

#include "vu1_flag_ring.h"

#if defined(_MSC_VER)
#define VUHOT_INLINE __forceinline
#else
#define VUHOT_INLINE inline __attribute__((always_inline))
#endif

namespace vuhot
{
    // PROBE-ABL (measurement only, inexact when set): PS2X_VU1_ABL bit0 = skip classify/MAC/STATUS in fmac, bit1 = skip operand normalisation in fmac
    inline const uint32_t g_abl = []() { const char *e = std::getenv("PS2X_VU1_ABL"); return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u; }();

    VUHOT_INLINE float N(float value) // VU1Interpreter::normalizeOperand
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

    VUHOT_INLINE __m128 N4(__m128 v) // normalizeOperand on 4 lanes
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

    // bit c of `bits` -> lane c all ones
    VUHOT_INLINE __m128i laneMask4(uint32_t bits)
    {
        const __m128i sel = _mm_set_epi32(8, 4, 2, 1);
        return _mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32(static_cast<int>(bits)), sel), sel);
    }

    constexpr uint32_t rev4(uint32_t b) { return ((b & 1u) << 3) | ((b & 2u) << 1) | ((b & 4u) >> 1) | ((b & 8u) >> 3); }

    // classification of the exact (double) result per lane; bit c = lane c. `under` excludes zero (fastUpper's flagsOf chain).
    struct Cls
    {
        uint32_t sign, zero, over, under;
    };

    VUHOT_INLINE Cls classify(__m256d ex)
    {
        Cls r;
        const __m256d mag = _mm256_and_pd(ex, _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFll)));
        r.sign = static_cast<uint32_t>(_mm256_movemask_pd(ex));
        r.zero = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(mag, _mm256_setzero_pd(), _CMP_EQ_OQ)));
        r.over = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(mag, _mm256_set1_pd(static_cast<double>(FLT_MAX)), _CMP_GT_OQ)));
        r.under = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(mag, _mm256_set1_pd(static_cast<double>(FLT_MIN)), _CMP_LT_OQ))) & ~r.zero & 0xFu;
        return r;
    }

    // MAC word (lane value 8>>c, groups zero/sign/underflow/overflow at <<0/4/8/12) and OR of the 4-bit lane flags over the dest lanes
    VUHOT_INLINE void macOf(const Cls &c, uint32_t d4, uint32_t &mac, uint32_t &status4)
    {
        const uint32_t z = (c.zero | c.under) & d4;
        const uint32_t s = c.sign & d4;
        const uint32_t u = c.under & d4;
        const uint32_t o = c.over & d4;
        mac = rev4(z) | (rev4(s) << 4) | (rev4(u) << 8) | (rev4(o) << 12);
        status4 = (z ? 1u : 0u) | (s ? 2u : 0u) | (u ? 4u : 0u) | (o ? 8u : 0u);
    }

    // kind: 0 add, 1 sub, 2 madd, 3 msub, 4 mul. src: 0 vt[BC], 1 q, 2 i, 3 vt[c]. DEST in VU encoding (x = 8).
    template <int KIND, int SRC, int BC, bool TOACC, int DEST, bool KEEPMAC>
    VUHOT_INLINE void fmac(VU1Interpreter &v, uint32_t unit, uint32_t fs, uint32_t ft, uint32_t fd)
    {
        if constexpr (DEST == 0)
            return; // updateFmacFlags returns early and no lane is written
        constexpr bool madd = KIND == 2 || KIND == 3;
        constexpr uint32_t D4 = rev4(static_cast<uint32_t>(DEST));
        VU1State &st = v.m_state;
        const __m128 a4 = (g_abl & 2u) ? _mm_loadu_ps(st.vf[fs]) : N4(_mm_loadu_ps(st.vf[fs]));
        __m128 b4;
        if constexpr (SRC == 3)
            b4 = N4(_mm_loadu_ps(st.vf[ft]));
        else
        {
            float bs;
            if constexpr (SRC == 0)
                bs = N(st.vf[ft][BC]);
            else if constexpr (SRC == 1)
                bs = N(st.q);
            else
                bs = N(st.i);
            b4 = _mm_set1_ps(bs);
        }
        // PERF-FC: the float result is computed first. When every destination lane of it (and, for madd/msub, of the product) has a biased
        // exponent in [2,253], the exact double result is a normal in-range number (rounding moves it by < 1 ulp), so zero/underflow/overflow
        // are all false and only the sign bits matter: the double-precision classification is skipped. Otherwise the original path runs.
        __m128 val;
        __m128 ac4 = _mm_setzero_ps();
        if constexpr (KIND == 0)
            val = _mm_add_ps(a4, b4);
        else if constexpr (KIND == 1)
            val = _mm_sub_ps(a4, b4);
        else if constexpr (KIND == 4)
            val = _mm_mul_ps(a4, b4);
        else
        {
            ac4 = N4(_mm_loadu_ps(st.acc));
            val = (KIND == 2) ? _mm_fmadd_ps(a4, b4, ac4) : _mm_fnmadd_ps(a4, b4, ac4);
        }
        const __m128i i2 = _mm_set1_epi32(2), i253 = _mm_set1_epi32(253), iFF = _mm_set1_epi32(0xFF);
        const __m128i ev = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(val), 23), iFF);
        uint32_t bad = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_cmplt_epi32(ev, i2), _mm_cmpgt_epi32(ev, i253)))));
        __m128 prodf = _mm_setzero_ps();
        if constexpr (madd)
        {
            prodf = _mm_mul_ps(a4, b4);
            const __m128i ep = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(prodf), 23), iFF);
            bad |= static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_cmplt_epi32(ep, i2), _mm_cmpgt_epi32(ep, i253)))));
        }
        if (g_abl & 1u) // PROBE-ABL
        {
            float *d0 = TOACC ? st.acc : st.vf[fd];
            _mm_storeu_ps(d0, _mm_blend_ps(_mm_loadu_ps(d0), val, D4));
            return;
        }
        uint32_t mac, status4, extra = 0u;
        __m128i vb = _mm_castps_si128(val);
        if ((bad & D4) == 0u)
        {
            const uint32_t s = static_cast<uint32_t>(_mm_movemask_ps(val)) & D4;
            mac = rev4(s) << 4;
            status4 = s ? 2u : 0u;
            if constexpr (madd)
                extra = (static_cast<uint32_t>(_mm_movemask_ps(prodf)) & D4) ? 2u : 0u;
        }
        else
        {
            const __m256d ad = _mm256_cvtps_pd(a4);
            const __m256d bd = _mm256_cvtps_pd(b4);
            __m256d exd;
            __m256d prod = _mm256_setzero_pd();
            if constexpr (KIND == 0)
                exd = _mm256_add_pd(ad, bd);
            else if constexpr (KIND == 1)
                exd = _mm256_sub_pd(ad, bd);
            else if constexpr (KIND == 4)
                exd = _mm256_mul_pd(ad, bd);
            else
            {
                const __m256d acd = _mm256_cvtps_pd(ac4);
                prod = _mm256_mul_pd(ad, bd);
                exd = (KIND == 2) ? _mm256_add_pd(acd, prod) : _mm256_sub_pd(acd, prod);
            }
            const Cls cx = classify(exd);
            const __m128i sg = _mm_and_si128(laneMask4(cx.sign), _mm_set1_epi32(static_cast<int>(0x80000000u)));
            vb = _mm_blendv_epi8(vb, sg, laneMask4(cx.zero | cx.under));
            vb = _mm_blendv_epi8(vb, _mm_or_si128(sg, _mm_set1_epi32(0x7F7FFFFF)), laneMask4(cx.over));
            uint32_t dummy;
            macOf(cx, D4, mac, status4);
            if constexpr (madd)
                macOf(classify(prod), D4, dummy, extra);
        }
        if constexpr (KEEPMAC)
            vuflags::flgPush(vuflags::s_flgMac[unit], v.m_cycle, mac);
        const uint32_t current = status4 & 0xFu;
        st.status = (st.status & 0xFF0u) | current | ((current | extra) << 6);
        float *dst = TOACC ? st.acc : st.vf[fd];
        _mm_storeu_ps(dst, _mm_blend_ps(_mm_loadu_ps(dst), _mm_castsi128_ps(vb), D4));
    }

    // MAX / MINI (no flags): src 0 = vt[BC], 2 = i, 3 = vt[c]
    template <bool IS_MAX, int SRC, int BC, int DEST>
    VUHOT_INLINE void maxmini(VU1State &st, uint32_t fs, uint32_t ft, uint32_t fd)
    {
        if constexpr (DEST == 0)
            return;
        constexpr uint32_t D4 = rev4(static_cast<uint32_t>(DEST));
        const __m128 a4 = N4(_mm_loadu_ps(st.vf[fs]));
        __m128 b4;
        if constexpr (SRC == 3)
            b4 = N4(_mm_loadu_ps(st.vf[ft]));
        else if constexpr (SRC == 0)
            b4 = _mm_set1_ps(N(st.vf[ft][BC]));
        else
            b4 = _mm_set1_ps(N(st.i));
        const __m128 res = IS_MAX ? _mm_max_ps(a4, b4) : _mm_min_ps(a4, b4); // == (a > b) ? a : b  /  (a < b) ? a : b
        float *dst = st.vf[fd];
        _mm_storeu_ps(dst, _mm_blend_ps(_mm_loadu_ps(dst), res, D4));
    }

    // FTOI0/4/12/15 (vuFloatToInt): destination is vf[ft]
    template <int SCALE, int DEST>
    VUHOT_INLINE void ftoi(VU1State &st, uint32_t fs, uint32_t ft)
    {
        if constexpr (DEST == 0)
            return;
        constexpr uint32_t D4 = rev4(static_cast<uint32_t>(DEST));
        const __m128 a4 = N4(_mm_loadu_ps(st.vf[fs]));
        const __m256d sc = _mm256_mul_pd(_mm256_cvtps_pd(a4), _mm256_set1_pd(static_cast<double>(static_cast<float>(SCALE))));
        __m128i t = _mm256_cvttpd_epi32(sc);
        const uint32_t ge = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(sc, _mm256_set1_pd(static_cast<double>(std::numeric_limits<int32_t>::max())), _CMP_GE_OQ)));
        const uint32_t le = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(sc, _mm256_set1_pd(static_cast<double>(std::numeric_limits<int32_t>::min())), _CMP_LE_OQ)));
        t = _mm_blendv_epi8(t, _mm_set1_epi32(std::numeric_limits<int32_t>::max()), laneMask4(ge));
        t = _mm_blendv_epi8(t, _mm_set1_epi32(std::numeric_limits<int32_t>::min()), laneMask4(le));
        float *dst = st.vf[ft];
        _mm_storeu_ps(dst, _mm_blend_ps(_mm_loadu_ps(dst), _mm_castsi128_ps(t), D4));
    }

    VUHOT_INLINE void applyDest(float *dst, const float *result, uint8_t dest)
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

    // PERF-XG: cycle advance without pipeline commits (the recorded block has none there) that still lets a running XGKICK transfer
    // consume its quadwords cycle by cycle, exactly like VU1Interpreter::advanceOneCycle does after its (empty) commit.
    VUHOT_INLINE void setCycleXg(VU1Interpreter &v, const VuNativeOps &op, uint64_t target)
    {
        if (!v.m_xgkick.active)
        {
            v.m_cycle = target;
            v.m_state.cycles = target;
            return;
        }
        while (v.m_cycle < target)
        {
            ++v.m_cycle;
            v.m_state.cycles = v.m_cycle;
            if (v.m_xgkick.active)
                op.xgProgress(v);
        }
    }

    // VU1Interpreter::queueStore for the native (probe-free) path: first free pending-store slot, ready next cycle
    VUHOT_INLINE void storeFast(VU1Interpreter &v, VuNativeCtx &c, uint32_t address, const uint32_t *words, uint8_t laneMask)
    {
        const uint32_t freeMask = ~(*c.storeMask) & 0xFFu;
        if (freeMask == 0u)
        {
            c.ops->qStore(v, address, words, laneMask); // reports the overflow like the interpreter
            return;
        }
        const uint32_t slot = static_cast<uint32_t>(std::countr_zero(freeMask));
        VU1Interpreter::PendingStore &s = v.m_storePipeline[slot];
        s.valid = true;
        *c.storeMask |= 1u << slot;
        *c.nextReadyW = 0u;
        s.readyCycle = v.m_cycle + 1u;
        s.address = address;
        s.laneMask = laneMask;
        s.words[0] = words[0];
        s.words[1] = words[1];
        s.words[2] = words[2];
        s.words[3] = words[3];
    }

    // ---- lower ops ----
    VUHOT_INLINE void lq(VU1State &st, const uint8_t *vuData, uint32_t dataSize, uint8_t dest, uint32_t it, uint32_t is, int16_t imm)
    {
        uint32_t addr = (static_cast<uint32_t>(static_cast<int32_t>(st.vi[is] + imm))) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest(st.vf[it], tmp, dest);
        }
    }

    template <typename QS>
    VUHOT_INLINE void sq(VU1State &st, uint32_t dataSize, uint8_t dest, uint32_t fs, uint32_t it, int16_t imm, QS &&qstore)
    {
        uint32_t addr = (static_cast<uint32_t>(static_cast<int32_t>(st.vi[it] + imm))) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            uint32_t words[4]{};
            std::memcpy(words, st.vf[fs], sizeof(words));
            qstore(addr, words, dest);
        }
    }

    VUHOT_INLINE void ilw(VU1State &st, const uint8_t *vuData, uint32_t dataSize, uint8_t dest, uint32_t it, uint32_t is, int16_t imm)
    {
        uint32_t addr = (static_cast<uint32_t>(static_cast<int32_t>(st.vi[is] + imm))) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            int comp = 3;
            if (dest & 0x8)
                comp = 0;
            else if (dest & 0x4)
                comp = 1;
            else if (dest & 0x2)
                comp = 2;
            uint32_t value;
            std::memcpy(&value, vuData + addr + comp * 4, 4);
            if (it != 0)
                st.vi[it] = static_cast<int32_t>(static_cast<int16_t>(value & 0xFFFF));
        }
    }

    template <typename QS>
    VUHOT_INLINE void isw(VU1State &st, uint32_t dataSize, uint8_t dest, uint32_t it, uint32_t is, int16_t imm, QS &&qstore)
    {
        uint32_t addr = (static_cast<uint32_t>(static_cast<int32_t>(st.vi[is] + imm))) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it] & 0xFFFF));
            const uint32_t words[4] = {val, val, val, val};
            qstore(addr, words, dest);
        }
    }

    VUHOT_INLINE void lqi(VU1State &st, const uint8_t *vuData, uint32_t dataSize, uint8_t dest, uint32_t vfT, uint32_t viS)
    {
        uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[viS]))) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest(st.vf[vfT], tmp, dest);
        }
        if (viS != 0)
            st.vi[viS] = static_cast<int16_t>(st.vi[viS] + 1);
    }

    template <typename QS>
    VUHOT_INLINE void sqi(VU1State &st, uint32_t dataSize, uint8_t dest, uint32_t vfS, uint32_t viT, QS &&qstore)
    {
        uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[viT]))) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            uint32_t words[4]{};
            std::memcpy(words, st.vf[vfS], sizeof(words));
            qstore(addr, words, dest);
        }
        if (viT != 0)
            st.vi[viT] = static_cast<int16_t>(st.vi[viT] + 1);
    }

    VUHOT_INLINE void move(VU1State &st, uint8_t dest, uint32_t vfT, uint32_t vfS)
    {
        float tmp[4];
        std::memcpy(tmp, st.vf[vfS], 16);
        applyDest(st.vf[vfT], tmp, dest);
    }

    VUHOT_INLINE void mr32(VU1State &st, uint8_t dest, uint32_t vfT, uint32_t vfS)
    {
        float tmp[4] = {st.vf[vfS][1], st.vf[vfS][2], st.vf[vfS][3], st.vf[vfS][0]};
        applyDest(st.vf[vfT], tmp, dest);
    }

    // branch-condition register read (VU1Interpreter::readBranchVi)
    VUHOT_INLINE int32_t readBranchVi(const VU1Interpreter &v, uint32_t reg)
    {
        if (reg == 0u)
            return 0;
        if (v.m_viBranchBackupValid && v.m_viBranchBackupReg == reg)
            return v.m_viBranchBackupValue;
        return v.m_state.vi[reg];
    }

    // ---- PERF-INL: inline copies of VU1Interpreter's DIV / SQRT / RSQRT / CLIP (see tools/vu1gen/patch_inline_hot.py) ----
    VUHOT_INLINE float normResult(float value) // VU1Interpreter::normalizeResult without the flag output
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u;
        const uint32_t magnitude = bits & 0x7FFFFFFFu;
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (magnitude != 0u)
        {
            if (exponent == 0u)
                bits = sign;
            else if (exponent == 0xFFu)
                bits = sign | 0x7F7FFFFFu;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // VU1Interpreter::queueQ (non-fast mode: the native path is never used in fast mode)
    VUHOT_INLINE void pushQ(VU1Interpreter &v, VuNativeCtx &c, float value, uint32_t latency, uint32_t statusDi)
    {
        v.m_fdiv.valid = true;
        *c.nextReadyW = 0u;
        v.m_fdiv.readyCycle = v.m_cycle + latency;
        v.m_fdiv.value = normResult(value);
        v.m_fdiv.statusDi = statusDi & 0x30u;
    }

    VUHOT_INLINE void divQ(VU1Interpreter &v, VuNativeCtx &c, uint32_t fs, uint32_t fsf, uint32_t ft, uint32_t ftf)
    {
        const float num = N(v.m_state.vf[fs][fsf]);
        const float den = N(v.m_state.vf[ft][ftf]);
        uint32_t statusDi = 0u;
        float result;
        if (den == 0.0f)
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        else
            result = num / den;
        pushQ(v, c, normResult(result), 7u, statusDi);
    }

    VUHOT_INLINE void sqrtQ(VU1Interpreter &v, VuNativeCtx &c, uint32_t ft, uint32_t ftf)
    {
        const float val = N(v.m_state.vf[ft][ftf]);
        pushQ(v, c, std::sqrt(std::fabs(val)), 7u, val < 0.0f ? 0x10u : 0u);
    }

    VUHOT_INLINE void rsqrtQ(VU1Interpreter &v, VuNativeCtx &c, uint32_t fs, uint32_t fsf, uint32_t ft, uint32_t ftf)
    {
        const float num = N(v.m_state.vf[fs][fsf]);
        const float radicand = N(v.m_state.vf[ft][ftf]);
        const float den = std::sqrt(std::fabs(radicand));
        uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
        float result;
        if (den != 0.0f)
            result = num / den;
        else
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        pushQ(v, c, normResult(result), 13u, statusDi);
    }

    // CLIP: VU1Interpreter::execUpper case 0x1F + queueClip (immediate-flags mode: ring push)
    VUHOT_INLINE void clip(VU1Interpreter &v, uint32_t fs, uint32_t ft)
    {
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &v.m_state.vf[ft][3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        const auto exceeds = [limit](float value, uint32_t signMask)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            bits ^= signMask;
            int32_t ordered = 0;
            std::memcpy(&ordered, &bits, sizeof(ordered));
            return ordered > limit;
        };
        uint32_t flags = 0u;
        if (exceeds(v.m_state.vf[fs][0], 0x00000000u)) flags |= 0x01u;
        if (exceeds(v.m_state.vf[fs][0], 0x80000000u)) flags |= 0x02u;
        if (exceeds(v.m_state.vf[fs][1], 0x00000000u)) flags |= 0x04u;
        if (exceeds(v.m_state.vf[fs][1], 0x80000000u)) flags |= 0x08u;
        if (exceeds(v.m_state.vf[fs][2], 0x00000000u)) flags |= 0x10u;
        if (exceeds(v.m_state.vf[fs][2], 0x80000000u)) flags |= 0x20u;
        v.m_workingClip = ((v.m_workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
        vuflags::flgPush(vuflags::s_flgClip[v.m_unit == VU1Interpreter::Unit::VU1 ? 1u : 0u], v.m_cycle, v.m_workingClip);
    }
} // namespace vuhot
