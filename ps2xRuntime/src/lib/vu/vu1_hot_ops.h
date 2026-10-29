// vu1_hot_ops.h -- inline, constant-folding op implementations for the generated VU1 hot blocks (generated/vu1_hot_blocks.cpp).
// Exact copies of the interpreter semantics: fmac == fastUpper (ps2_vu1_core.cpp), the rest == ps2_vu1_upper.cpp / ps2_vu1_lower.cpp.
// The including TU must have done `#define private public` + #include "runtime/ps2_vu1.h" (VU1Interpreter, VU1State).
#pragma once

#include <cfloat>
#include <cmath>
#include <cstdint>
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

    // fastUpper's flagsOf: long double == double on MSVC
    VUHOT_INLINE uint8_t flagsOf(float &value, double ex)
    {
        const bool negative = std::signbit(ex);
        const double magnitude = std::fabs(ex);
        uint8_t flags = negative ? 0x2u : 0u;
        uint32_t bits = negative ? 0x80000000u : 0u;
        if (magnitude == 0.0)
        {
            flags |= 0x1u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude > static_cast<double>(FLT_MAX))
        {
            flags |= 0x8u;
            bits |= 0x7F7FFFFFu;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude < static_cast<double>(FLT_MIN))
        {
            flags |= 0x5u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        return flags;
    }

    // VU1Interpreter::updateFmacFlags, immediate-flag model (PERF-HV)
    VUHOT_INLINE void pushFmacFlags(VU1State &st, uint32_t unit, uint64_t cycle, const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky)
    {
        if (dest == 0u)
            return;
        uint32_t mac = 0u;
        uint32_t status = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint8_t lane = static_cast<uint8_t>(8u >> c);
            if ((dest & lane) == 0u)
                continue;
            const uint32_t flags = laneFlags[c];
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
        vuflags::flgPush(vuflags::s_flgMac[unit], cycle, mac);
        vuflags::s_flgStatusUndo[unit] = {cycle, st.status, true};
        const uint32_t current = status & 0xFu;
        st.status = (st.status & 0xFF0u) | current | ((current | extraSticky) << 6);
    }

    // kind: 0 add, 1 sub, 2 madd, 3 msub, 4 mul. src: 0 vt[BC], 1 q, 2 i, 3 vt[c]. Same math as fastUpper.
    template <int KIND, int SRC, int BC, bool TOACC>
    VUHOT_INLINE void fmac(VU1Interpreter &v, uint32_t unit, uint8_t dest, uint32_t fs, uint32_t ft, uint32_t fd)
    {
        VU1State &st = v.m_state;
        constexpr bool madd = KIND == 2 || KIND == 3;
        float bval = 0.0f;
        if constexpr (SRC == 0)
            bval = N(st.vf[ft][BC]);
        else if constexpr (SRC == 1)
            bval = N(st.q);
        else if constexpr (SRC == 2)
            bval = N(st.i);

        float result[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint8_t laneFlags[4] = {0u, 0u, 0u, 0u};
        uint32_t extraSticky = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if ((dest & (8u >> c)) == 0u)
                continue;
            const float a = N(st.vf[fs][c]);
            const float b = (SRC == 3) ? N(st.vf[ft][c]) : bval;
            const float ac = madd ? N(st.acc[c]) : 0.0f;
            float val;
            double ex;
            if constexpr (KIND == 0)
            {
                val = a + b;
                ex = static_cast<double>(a) + static_cast<double>(b);
            }
            else if constexpr (KIND == 1)
            {
                val = a - b;
                ex = static_cast<double>(a) - static_cast<double>(b);
            }
            else if constexpr (KIND == 2)
            {
                val = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac)));
                ex = static_cast<double>(ac) + static_cast<double>(a) * static_cast<double>(b);
            }
            else if constexpr (KIND == 3)
            {
                val = _mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(ac)));
                ex = static_cast<double>(ac) - static_cast<double>(a) * static_cast<double>(b);
            }
            else
            {
                val = a * b;
                ex = static_cast<double>(a) * static_cast<double>(b);
            }
            laneFlags[c] = flagsOf(val, ex);
            result[c] = val;
            if constexpr (madd)
            {
                float pv = a * b;
                extraSticky |= flagsOf(pv, static_cast<double>(a) * static_cast<double>(b)) & 0xFu;
            }
        }
        pushFmacFlags(st, unit, v.m_cycle, laneFlags, dest, extraSticky);
        float *dst = TOACC ? st.acc : st.vf[fd];
        for (uint32_t c = 0; c < 4u; ++c)
            if ((dest & (8u >> c)) != 0u)
                dst[c] = result[c];
    }

    // MAX / MINI (no flags): src 0 = vt[BC], 2 = i, 3 = vt[c]
    template <bool IS_MAX, int SRC, int BC>
    VUHOT_INLINE void maxmini(VU1State &st, uint8_t dest, uint32_t fs, uint32_t ft, uint32_t fd)
    {
        float bval = 0.0f;
        if constexpr (SRC == 0)
            bval = N(st.vf[ft][BC]);
        else if constexpr (SRC == 2)
            bval = N(st.i);
        float result[4];
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const float a = N(st.vf[fs][c]);
            const float b = (SRC == 3) ? N(st.vf[ft][c]) : bval;
            result[c] = IS_MAX ? ((a > b) ? a : b) : ((a < b) ? a : b);
        }
        for (uint32_t c = 0; c < 4u; ++c)
            if ((dest & (8u >> c)) != 0u)
                st.vf[fd][c] = result[c];
    }

    VUHOT_INLINE int32_t floatToInt(float value, float scale) // vuFloatToInt
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    // FTOI0/4/12/15: destination is vf[ft]
    template <int SCALE>
    VUHOT_INLINE void ftoi(VU1State &st, uint8_t dest, uint32_t fs, uint32_t ft)
    {
        float result[4];
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const int32_t iv = floatToInt(N(st.vf[fs][c]), static_cast<float>(SCALE));
            std::memcpy(&result[c], &iv, 4);
        }
        for (uint32_t c = 0; c < 4u; ++c)
            if ((dest & (8u >> c)) != 0u)
                st.vf[ft][c] = result[c];
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
} // namespace vuhot
