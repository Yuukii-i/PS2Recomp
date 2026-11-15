// vu0_func.inc -- Task V0G: functional (decompiled) VU0 handlers for the 6 hot entries
// (PS2X_VU0_FUNC=1 enables, default OFF; PS2X_VU0_FUNC_CHECK=1 runs both and compares).
// Included by ps2_runtime.cpp right after vu/vu0_fast.inc. No header edits.

namespace vu0func
{
constexpr uint64_t kTargetImageHash = 0xd985722524327b89ull; // PERF-V0G

using FuncHandler = void (*)(R5900Context *ctx); // PERF-V0G

inline bool isFuncEnabled() // PERF-V0G
{
    static const bool s_on = []()
    {
        const char *e = std::getenv("PS2X_VU0_FUNC");
        return !(e && e[0] == '0');
    }();
    return s_on;
}

inline bool isFuncCheckEnabled() // PERF-V0G
{
    static const bool s_on = []()
    {
        const char *e = std::getenv("PS2X_VU0_FUNC_CHECK");
        return e && e[0] == '1';
    }();
    return s_on;
}

constexpr vu0fast::DecodedOp mkOp(uint8_t kind, uint8_t src, uint8_t bc, uint8_t toAcc,
                                   uint8_t dest, uint8_t fs, uint8_t ft, uint8_t fd) // PERF-V0G
{
    return vu0fast::DecodedOp{kind, src, bc, toAcc, dest, fs, ft, fd};
}

constexpr vu0fast::DecodedLower mkRsqrt(uint8_t vfS, uint8_t vfT, uint8_t fsf, uint8_t ftf) // PERF-V0G
{
    return vu0fast::DecodedLower{vu0fast::LOP_RSQRT, vfS, vfT, fsf, ftf, 0, 0};
}

inline uint32_t maskVu0R(__m128 r) // PERF-V0G
{
    const uint32_t r0 = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(r)));
    return 0x3F800000u | (r0 & 0x007FFFFFu);
}

inline void finalizeCommon(R5900Context *ctx, __m128 vf0, uint32_t mac, uint32_t status,
                           uint32_t endPC, uint32_t clipFlags) // PERF-V0G
{
    ctx->vu0_vf[0] = vf0;
    ctx->vi[0] = 0;
    ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(maskVu0R(ctx->vu0_r))));
    ctx->vu0_mac_flags = mac;
    ctx->vu0_status = static_cast<uint16_t>(status);
    ctx->vu0_clip_flags = clipFlags;
    ctx->vu0_clip_flags2 = clipFlags;
    ctx->vu0_pc = endPC;
    ctx->vu0_tpc = endPC;
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u);
    ctx->vu0_vpu_stat2 = 0u;
}

// PERF-V0H: State updated per op by vu0fast::execFmacFast<ComputeMac>:
//   1. Destination register: vf[op.fd] (if !op.toAcc && op.fd != 0) or vacc + dirtyAcc=true (if op.toAcc),
//      masked by sseDestMask = kBitRev4[op.dest].
//   2. stickyFlags (bits 0..3: Z=1, S=2, U=4, O=8): ORed across enabled lanes of dex and, for
//      OP_MADD/OP_MSUB, the intermediate product dprod. Shifted into vu0_status[9:6] at program end.
//   3. When ComputeMac == true (only needed on the final FMAC op): mac (16-bit Z/S/U/O per lane via
//      kBitRev4) and status[3:0] (OR of enabled lanes' Z/S/U/O of dex).
//   Note: execFmacFast does not touch FlgRing, vu0_clip_flags, vu0_q, vu0_r, vi, or cycle counts.

struct V0hState // PERF-V0H
{
    __m256d allSafe = _mm256_castsi256_pd(_mm256_set1_epi64x(-1LL));
    __m256d stickyZero = _mm256_setzero_pd();
    __m256d stickySign = _mm256_setzero_pd();
};

template <uint32_t SseMask>
inline __m256d v0hLaneMaskD() // PERF-V0H
{
    return _mm256_castsi256_pd(_mm256_set_epi64x(
        (SseMask & 8u) ? -1LL : 0LL,
        (SseMask & 4u) ? -1LL : 0LL,
        (SseMask & 2u) ? -1LL : 0LL,
        (SseMask & 1u) ? -1LL : 0LL));
}

template <uint32_t SseMask>
inline __m256d v0hInvLaneMaskD() // PERF-V0H
{
    return _mm256_castsi256_pd(_mm256_set_epi64x(
        (SseMask & 8u) ? 0LL : -1LL,
        (SseMask & 4u) ? 0LL : -1LL,
        (SseMask & 2u) ? 0LL : -1LL,
        (SseMask & 1u) ? 0LL : -1LL));
}

template <uint32_t SseMask = 0xFu>
inline void v0hTrackNoAcc(__m256d dex, V0hState &st) // PERF-V0H
{
    const __m256d absMaskD = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    const __m256d zeroD = _mm256_setzero_pd();
    const __m256d fltMinD = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
    const __m256d fltMaxD = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));

    const __m256d absEx = _mm256_and_pd(dex, absMaskD);
    const __m256d exIsZero = _mm256_cmp_pd(absEx, zeroD, _CMP_EQ_OQ);
    const __m256d exSafe = _mm256_or_pd(
        exIsZero,
        _mm256_and_pd(_mm256_cmp_pd(absEx, fltMinD, _CMP_GE_OQ),
                      _mm256_cmp_pd(absEx, fltMaxD, _CMP_LE_OQ)));
    if constexpr (SseMask == 0xFu)
    {
        st.allSafe = _mm256_and_pd(st.allSafe, exSafe);
        st.stickyZero = _mm256_or_pd(st.stickyZero, exIsZero);
        st.stickySign = _mm256_or_pd(st.stickySign, dex);
    }
    else
    {
        const __m256d en = v0hLaneMaskD<SseMask>();
        st.allSafe = _mm256_and_pd(st.allSafe, _mm256_or_pd(exSafe, v0hInvLaneMaskD<SseMask>()));
        st.stickyZero = _mm256_or_pd(st.stickyZero, _mm256_and_pd(exIsZero, en));
        st.stickySign = _mm256_or_pd(st.stickySign, _mm256_and_pd(dex, en));
    }
}

template <uint32_t SseMask = 0xFu>
inline void v0hTrackAcc(__m256d dex, __m256d dprod, V0hState &st) // PERF-V0H
{
    const __m256d absMaskD = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    const __m256d zeroD = _mm256_setzero_pd();
    const __m256d fltMinD = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
    const __m256d fltMaxD = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));

    const __m256d absEx = _mm256_and_pd(dex, absMaskD);
    const __m256d exIsZero = _mm256_cmp_pd(absEx, zeroD, _CMP_EQ_OQ);
    const __m256d exSafe = _mm256_or_pd(
        exIsZero,
        _mm256_and_pd(_mm256_cmp_pd(absEx, fltMinD, _CMP_GE_OQ),
                      _mm256_cmp_pd(absEx, fltMaxD, _CMP_LE_OQ)));

    const __m256d absProd = _mm256_and_pd(dprod, absMaskD);
    const __m256d prodIsZero = _mm256_cmp_pd(absProd, zeroD, _CMP_EQ_OQ);
    const __m256d prodSafe = _mm256_or_pd(
        prodIsZero,
        _mm256_and_pd(_mm256_cmp_pd(absProd, fltMinD, _CMP_GE_OQ),
                      _mm256_cmp_pd(absProd, fltMaxD, _CMP_LE_OQ)));

    const __m256d safeD = _mm256_and_pd(exSafe, prodSafe);
    const __m256d zAny = _mm256_or_pd(exIsZero, prodIsZero);
    const __m256d sAny = _mm256_or_pd(dex, dprod);

    if constexpr (SseMask == 0xFu)
    {
        st.allSafe = _mm256_and_pd(st.allSafe, safeD);
        st.stickyZero = _mm256_or_pd(st.stickyZero, zAny);
        st.stickySign = _mm256_or_pd(st.stickySign, sAny);
    }
    else
    {
        const __m256d en = v0hLaneMaskD<SseMask>();
        st.allSafe = _mm256_and_pd(st.allSafe, _mm256_or_pd(safeD, v0hInvLaneMaskD<SseMask>()));
        st.stickyZero = _mm256_or_pd(st.stickyZero, _mm256_and_pd(zAny, en));
        st.stickySign = _mm256_or_pd(st.stickySign, _mm256_and_pd(sAny, en));
    }
}

template <uint32_t LastSseMask = 0xFu>
inline bool v0hFinalizeFlags(const V0hState &st, __m256d lastDex, uint32_t &mac, uint32_t &status) // PERF-V0H
{
    if (_mm256_movemask_pd(st.allSafe) != 0xF)
        return false;
    const __m256d absMaskD = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    const __m256d lastExIsZero = _mm256_cmp_pd(_mm256_and_pd(lastDex, absMaskD), _mm256_setzero_pd(), _CMP_EQ_OQ);
    const uint32_t lastZeroSse = static_cast<uint32_t>(_mm256_movemask_pd(lastExIsZero)) & LastSseMask;
    const uint32_t lastSignSse = static_cast<uint32_t>(_mm256_movemask_pd(lastDex)) & LastSseMask;
    mac = vu0fast::kBitRev4[lastZeroSse] | (static_cast<uint32_t>(vu0fast::kBitRev4[lastSignSse]) << 4);
    const uint32_t current = ((lastZeroSse != 0u) ? 1u : 0u) | ((lastSignSse != 0u) ? 2u : 0u);
    const uint32_t stickyZ = (_mm256_movemask_pd(st.stickyZero) != 0) ? 1u : 0u;
    const uint32_t stickyS = (_mm256_movemask_pd(st.stickySign) != 0) ? 2u : 0u;
    status = (status & 0xFF0u) | current | ((stickyZ | stickyS) << 6);
    return true;
}

inline __m256d v0hTransformVec(__m256d d4, __m256d d5, __m256d d6, __m256d d7, __m256d dv,
                               __m128 &outVf, __m128 &vacc, V0hState &st) // PERF-V0H
{
    const __m256d bx = _mm256_permute4x64_pd(dv, _MM_SHUFFLE(0, 0, 0, 0));
    const __m256d by = _mm256_permute4x64_pd(dv, _MM_SHUFFLE(1, 1, 1, 1));
    const __m256d bz = _mm256_permute4x64_pd(dv, _MM_SHUFFLE(2, 2, 2, 2));
    const __m256d bw = _mm256_permute4x64_pd(dv, _MM_SHUFFLE(3, 3, 3, 3));

    __m256d dex = _mm256_mul_pd(d4, bx);
    v0hTrackNoAcc(dex, st);
    __m256d dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    __m256d dprod = _mm256_mul_pd(d5, by);
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    dprod = _mm256_mul_pd(d6, bz);
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    vacc = _mm256_cvtpd_ps(dex);
    dac = _mm256_cvtps_pd(vacc);

    dprod = _mm256_mul_pd(d7, bw);
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    outVf = _mm256_cvtpd_ps(dex);
    return dex;
}

inline __m256d v0hScaleAndMatVec(__m256d da0, __m256d da1, __m256d ds,
                                  __m256d dt0, __m256d dt1,
                                  __m256d dm0, __m256d dm1, __m256d dm2, __m256d dm3,
                                  __m256d dm4, __m256d dm5, __m256d dm6,
                                  __m128 &outA0, __m128 &outA1, __m128 &outT0, __m128 &outT1,
                                  __m128 &vacc, V0hState &st) // PERF-V0H
{
    const __m256d dsx = _mm256_permute4x64_pd(ds, _MM_SHUFFLE(0, 0, 0, 0));
    const __m256d dOne = _mm256_set1_pd(1.0);

    __m256d dex = _mm256_mul_pd(da0, dsx);
    v0hTrackNoAcc(dex, st);
    outA0 = _mm256_cvtpd_ps(dex);
    const __m256d sa0 = _mm256_cvtps_pd(outA0);

    dex = _mm256_mul_pd(da1, dsx);
    v0hTrackNoAcc(dex, st);
    outA1 = _mm256_cvtpd_ps(dex);
    const __m256d sa1 = _mm256_cvtps_pd(outA1);

    dex = _mm256_mul_pd(dt0, dOne);
    v0hTrackNoAcc(dex, st);
    __m256d dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    __m256d dprod = _mm256_mul_pd(dm0, _mm256_permute4x64_pd(sa0, _MM_SHUFFLE(0, 0, 0, 0)));
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    dprod = _mm256_mul_pd(dm1, _mm256_permute4x64_pd(sa0, _MM_SHUFFLE(1, 1, 1, 1)));
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    dprod = _mm256_mul_pd(dm2, _mm256_permute4x64_pd(sa0, _MM_SHUFFLE(2, 2, 2, 2)));
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    dprod = _mm256_mul_pd(dm3, _mm256_permute4x64_pd(sa0, _MM_SHUFFLE(3, 3, 3, 3)));
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    outT0 = _mm256_cvtpd_ps(dex);

    dex = _mm256_mul_pd(dm4, _mm256_permute4x64_pd(sa1, _MM_SHUFFLE(0, 0, 0, 0)));
    v0hTrackNoAcc(dex, st);
    dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    dprod = _mm256_mul_pd(dm5, _mm256_permute4x64_pd(sa1, _MM_SHUFFLE(1, 1, 1, 1)));
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    dac = _mm256_cvtps_pd(_mm256_cvtpd_ps(dex));

    dprod = _mm256_mul_pd(dm6, _mm256_permute4x64_pd(sa1, _MM_SHUFFLE(2, 2, 2, 2)));
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    vacc = _mm256_cvtpd_ps(dex);
    dac = _mm256_cvtps_pd(vacc);

    dprod = _mm256_mul_pd(dt1, dOne);
    dex = _mm256_add_pd(dac, dprod);
    v0hTrackAcc(dex, dprod, st);
    outT1 = _mm256_cvtpd_ps(dex);
    return dex;
}

inline void v0hLenSq(__m256d dSrc, __m128 &vfDst, V0hState &st) // PERF-V0H
{
    __m256d dex = _mm256_mul_pd(dSrc, dSrc);
    v0hTrackNoAcc<0x7u>(dex, st);
    vfDst = _mm_blend_ps(vfDst, _mm256_cvtpd_ps(dex), 0x7);

    __m256d da = _mm256_cvtps_pd(vfDst);
    dex = _mm256_add_pd(da, _mm256_permute4x64_pd(da, _MM_SHUFFLE(1, 1, 1, 1)));
    v0hTrackNoAcc<0x1u>(dex, st);
    vfDst = _mm_move_ss(vfDst, _mm256_cvtpd_ps(dex));

    da = _mm256_cvtps_pd(vfDst);
    dex = _mm256_add_pd(da, _mm256_permute4x64_pd(da, _MM_SHUFFLE(2, 2, 2, 2)));
    v0hTrackNoAcc<0x1u>(dex, st);
    vfDst = _mm_move_ss(vfDst, _mm256_cvtpd_ps(dex));
}

// PERF-V0G / PERF-V0H: 000..080 (4x4 transform of vf8..vf11 by vf4..vf7)
inline void run_000(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    const __m128 vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;

    { // PERF-V0H: vectorized sticky-only fast path
        using namespace vu0fast;
        const __m256d d4 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[4]));
        const __m256d d5 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[5]));
        const __m256d d6 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[6]));
        const __m256d d7 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[7]));
        __m128 out8, out9, out10, out11, vacc;
        V0hState st;
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[8])),  out8,  vacc, st);
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[9])),  out9,  vacc, st);
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[10])), out10, vacc, st);
        const __m256d lastDex = v0hTransformVec(
            d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[11])), out11, vacc, st);
        if (v0hFinalizeFlags(st, lastDex, mac, status))
        {
            ctx->vu0_vf[8]  = out8;
            ctx->vu0_vf[9]  = out9;
            ctx->vu0_vf[10] = out10;
            ctx->vu0_vf[11] = out11;
            ctx->vu0_acc = vacc;
            finalizeCommon(ctx, vf0, mac, status, 0x088u, ctx->vu0_clip_flags);
            if (changeCsr)
                _mm_setcsr(prevCsr);
            return;
        }
    }

    __m128 vf[32];
    vf[0] = vf0;
    for (int i = 4; i <= 11; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    uint32_t stickyFlags = 0u;
    const __m128 vzero = _mm_setzero_ps();

    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4,  8, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5,  8, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6,  8, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7,  8, 8), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4,  9, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5,  9, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6,  9, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7,  9, 9), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4, 10, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5, 10, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6, 10, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7, 10,10), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4, 11, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5, 11, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6, 11, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<true> (mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7, 11,11), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    status |= (stickyFlags & 0xFu) << 6;
    for (int i = 8; i <= 11; ++i)
        ctx->vu0_vf[i] = vf[i];
    if (dirtyAcc)
        ctx->vu0_acc = vacc;
    finalizeCommon(ctx, vf[0], mac, status, 0x088u, ctx->vu0_clip_flags);

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

// PERF-V0G / PERF-V0H: 088..0E8 (scale vf4,vf6 by vf3.x + matrix-vec multiply into vf5,vf7)
inline void run_088(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    const __m128 vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;

    { // PERF-V0H: vectorized sticky-only fast path
        using namespace vu0fast;
        __m128 out4, out6, out5, out7, vacc;
        V0hState st;
        const __m256d lastDex = v0hScaleAndMatVec(
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[4])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[6])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[3])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[5])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[7])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[8])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[9])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[10])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[11])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[12])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[13])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[14])),
            out4, out6, out5, out7, vacc, st);
        if (v0hFinalizeFlags(st, lastDex, mac, status))
        {
            ctx->vu0_vf[4] = out4;
            ctx->vu0_vf[5] = out5;
            ctx->vu0_vf[6] = out6;
            ctx->vu0_vf[7] = out7;
            ctx->vu0_acc = vacc;
            finalizeCommon(ctx, vf0, mac, status, 0x0F0u, ctx->vu0_clip_flags);
            if (changeCsr)
                _mm_setcsr(prevCsr);
            return;
        }
    }

    __m128 vf[32];
    vf[0] = vf0;
    for (int i = 3; i <= 14; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    uint32_t stickyFlags = 0u;
    const __m128 vzero = _mm_setzero_ps();

    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 0, 0xF,  4, 3, 4), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 0, 0xF,  6, 3, 6), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF,  5, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 0, 1, 0xF,  8, 4, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF,  9, 4, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 10, 4, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 11, 4, 5), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 12, 6, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 13, 6, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 14, 6, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<true> (mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF,  7, 0, 7), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    status |= (stickyFlags & 0xFu) << 6;
    ctx->vu0_vf[4] = vf[4];
    ctx->vu0_vf[5] = vf[5];
    ctx->vu0_vf[6] = vf[6];
    ctx->vu0_vf[7] = vf[7];
    if (dirtyAcc)
        ctx->vu0_acc = vacc;
    finalizeCommon(ctx, vf[0], mac, status, 0x0F0u, ctx->vu0_clip_flags);

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

// PERF-V0G / PERF-V0H: CD0..D30 (scale vf1,vf15 by vf3.x + matrix-vec multiply into vf2,vf16)
inline void run_cd0(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    const __m128 vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;

    { // PERF-V0H: vectorized sticky-only fast path
        using namespace vu0fast;
        __m128 out1, out15, out2, out16, vacc;
        V0hState st;
        const __m256d lastDex = v0hScaleAndMatVec(
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[1])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[15])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[3])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[2])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[16])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[8])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[9])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[10])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[11])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[12])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[13])),
            _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[14])),
            out1, out15, out2, out16, vacc, st);
        if (v0hFinalizeFlags(st, lastDex, mac, status))
        {
            ctx->vu0_vf[1]  = out1;
            ctx->vu0_vf[2]  = out2;
            ctx->vu0_vf[15] = out15;
            ctx->vu0_vf[16] = out16;
            ctx->vu0_acc = vacc;
            finalizeCommon(ctx, vf0, mac, status, 0xD38u, ctx->vu0_clip_flags);
            if (changeCsr)
                _mm_setcsr(prevCsr);
            return;
        }
    }

    __m128 vf[32];
    vf[0] = vf0;
    vf[1] = ctx->vu0_vf[1];
    vf[2] = ctx->vu0_vf[2];
    vf[3] = ctx->vu0_vf[3];
    for (int i = 8; i <= 16; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    uint32_t stickyFlags = 0u;
    const __m128 vzero = _mm_setzero_ps();

    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 0, 0xF,  1,  3,  1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 0, 0xF, 15,  3, 15), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF,  2,  0,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 0, 1, 0xF,  8,  1,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF,  9,  1,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 10,  1,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 11,  1,  2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 12, 15,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 13, 15,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 14, 15,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<true> (mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 16,  0, 16), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    status |= (stickyFlags & 0xFu) << 6;
    ctx->vu0_vf[1] = vf[1];
    ctx->vu0_vf[2] = vf[2];
    ctx->vu0_vf[15] = vf[15];
    ctx->vu0_vf[16] = vf[16];
    if (dirtyAcc)
        ctx->vu0_acc = vacc;
    finalizeCommon(ctx, vf[0], mac, status, 0xD38u, ctx->vu0_clip_flags);

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

// PERF-V0G / PERF-V0H: 0F0..1D8 (4x4 transform + RSQRT normalization of vf8..vf10 into vf12..vf14)
inline void run_0f0(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    const __m128 vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;

    { // PERF-V0H: vectorized sticky-only fast path
        using namespace vu0fast;
        const __m256d d4 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[4]));
        const __m256d d5 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[5]));
        const __m256d d6 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[6]));
        const __m256d d7 = _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[7]));
        __m128 out8, out9, out10, out11, vacc;
        V0hState st;
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[8])),  out8,  vacc, st);
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[9])),  out9,  vacc, st);
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[10])), out10, vacc, st);
        v0hTransformVec(d4, d5, d6, d7, _mm256_cvtps_pd(normalizeOperand4(ctx->vu0_vf[11])), out11, vacc, st);

        __m128 out1 = ctx->vu0_vf[1];
        __m128 out2 = ctx->vu0_vf[2];
        __m128 out3 = ctx->vu0_vf[3];
        const __m256d d8 = _mm256_cvtps_pd(out8);
        const __m256d d9 = _mm256_cvtps_pd(out9);
        const __m256d d10 = _mm256_cvtps_pd(out10);
        v0hLenSq(d8,  out1, st);
        v0hLenSq(d9,  out3, st);
        v0hLenSq(d10, out2, st);

        float q = 0.0f;
        float fdivValue = 0.0f;
        uint32_t fdivStatusDi = 0u;

        execDivRsqrtStep(mkRsqrt(0, 1, 3, 0), vf0, out1, fdivValue, fdivStatusDi);
        q = fdivValue;
        status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
        __m256d dex = _mm256_mul_pd(d8, _mm256_set1_pd(static_cast<double>(normalizeOperand(q))));
        v0hTrackNoAcc<0x7u>(dex, st);
        const __m128 out12 = _mm_blend_ps(ctx->vu0_vf[12], _mm256_cvtpd_ps(dex), 0x7);

        execDivRsqrtStep(mkRsqrt(0, 3, 3, 0), vf0, out3, fdivValue, fdivStatusDi);
        q = fdivValue;
        status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
        dex = _mm256_mul_pd(d9, _mm256_set1_pd(static_cast<double>(normalizeOperand(q))));
        v0hTrackNoAcc<0x7u>(dex, st);
        const __m128 out13 = _mm_blend_ps(ctx->vu0_vf[13], _mm256_cvtpd_ps(dex), 0x7);

        execDivRsqrtStep(mkRsqrt(0, 2, 3, 0), vf0, out2, fdivValue, fdivStatusDi);
        q = fdivValue;
        status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
        const __m256d lastDex = _mm256_mul_pd(d10, _mm256_set1_pd(static_cast<double>(normalizeOperand(q))));
        v0hTrackNoAcc<0x7u>(lastDex, st);
        const __m128 out14 = _mm_blend_ps(ctx->vu0_vf[14], _mm256_cvtpd_ps(lastDex), 0x7);

        if (v0hFinalizeFlags<0x7u>(st, lastDex, mac, status))
        {
            ctx->vu0_vf[1]  = out1;
            ctx->vu0_vf[2]  = out2;
            ctx->vu0_vf[3]  = out3;
            ctx->vu0_vf[8]  = out8;
            ctx->vu0_vf[9]  = out9;
            ctx->vu0_vf[10] = out10;
            ctx->vu0_vf[11] = out11;
            ctx->vu0_vf[12] = out12;
            ctx->vu0_vf[13] = out13;
            ctx->vu0_vf[14] = out14;
            ctx->vu0_acc = vacc;
            ctx->vu0_q = q;
            finalizeCommon(ctx, vf0, mac, status, 0x1E0u, ctx->vu0_clip_flags);
            if (changeCsr)
                _mm_setcsr(prevCsr);
            return;
        }
        status = ctx->vu0_status;
    }

    __m128 vf[32];
    vf[0] = vf0;
    for (int i = 1; i <= 14; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    float q = ctx->vu0_q;
    __m128 vq = _mm_set1_ps(vu0fast::normalizeOperand(q));
    const __m128 vzero = _mm_setzero_ps();
    uint32_t stickyFlags = 0u;
    float fdivValue = 0.0f;
    uint32_t fdivStatusDi = 0u;

    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC,  0, 1, 0xF,  4,  8,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  1, 1, 0xF,  5,  8,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  2, 1, 0xF,  6,  8,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  3, 0, 0xF,  7,  8,  8), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC,  0, 1, 0xF,  4,  9,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  1, 1, 0xF,  5,  9,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  2, 1, 0xF,  6,  9,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_VEC, 0, 0, 0xE,  8,  8,  1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  3, 0, 0xF,  7,  9,  9), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC,  0, 1, 0xF,  4, 10,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  1, 1, 0xF,  5, 10,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_ADD,  SRC_VT_BC,  1, 0, 0x8,  1,  1,  1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_VEC, 0, 0, 0xE,  9,  9,  3), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  2, 1, 0xF,  6, 10,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  3, 0, 0xF,  7, 10, 10), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    execFmacFast<false>(mkOp(OP_ADD,  SRC_VT_BC,  2, 0, 0x8,  1,  1,  1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_ADD,  SRC_VT_BC,  1, 0, 0x8,  3,  3,  3), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC,  0, 1, 0xF,  4, 11,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_VEC, 0, 0, 0xE, 10, 10,  2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  1, 1, 0xF,  5, 11,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execDivRsqrtStep(mkRsqrt(0, 1, 3, 0), vf[0], vf[1], fdivValue, fdivStatusDi);

    execFmacFast<false>(mkOp(OP_ADD,  SRC_VT_BC,  2, 0, 0x8,  3,  3,  3), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  2, 1, 0xF,  6, 11,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execFmacFast<false>(mkOp(OP_ADD,  SRC_VT_BC,  1, 0, 0x8,  2,  2,  2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    // 1A8: WAITQ commits 1st RSQRT
    q = fdivValue;
    vq = _mm_set1_ps(normalizeOperand(q));
    status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC,  3, 0, 0xF,  7, 11, 11), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    // 1B0: MULq.xyz vf12, vf8, vf0 | RSQRT Q, vf0.w, vf3.x
    execFmacFast<false>(mkOp(OP_MUL,  SRC_Q,      0, 0, 0xE,  8,  0, 12), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execDivRsqrtStep(mkRsqrt(0, 3, 3, 0), vf[0], vf[3], fdivValue, fdivStatusDi);

    // 1B8: ADDz.x vf2, vf2, vf2 | WAITQ (commits 2nd RSQRT)
    q = fdivValue;
    vq = _mm_set1_ps(normalizeOperand(q));
    status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);
    execFmacFast<false>(mkOp(OP_ADD,  SRC_VT_BC,  2, 0, 0x8,  2,  2,  2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    // 1C0: MULq.xyz vf13, vf9, vf0 | RSQRT Q, vf0.w, vf2.x
    execFmacFast<false>(mkOp(OP_MUL,  SRC_Q,      0, 0, 0xE,  9,  0, 13), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);
    execDivRsqrtStep(mkRsqrt(0, 2, 3, 0), vf[0], vf[2], fdivValue, fdivStatusDi);

    // 1C8: NOP | WAITQ (commits 3rd RSQRT)
    q = fdivValue;
    vq = _mm_set1_ps(normalizeOperand(q));
    status = (status & 0xFCFu) | fdivStatusDi | (fdivStatusDi << 6);

    // 1D0: MULq.xyz vf14, vf10, vf0 [E]
    execFmacFast<true> (mkOp(OP_MUL,  SRC_Q,      0, 0, 0xE, 10,  0, 14), vf, vacc, dirtyAcc, stickyFlags, mac, status, vq, vzero);

    status |= (stickyFlags & 0xFu) << 6;
    for (int i = 1; i <= 3; ++i)
        ctx->vu0_vf[i] = vf[i];
    for (int i = 8; i <= 14; ++i)
        ctx->vu0_vf[i] = vf[i];
    if (dirtyAcc)
        ctx->vu0_acc = vacc;
    ctx->vu0_q = q;
    finalizeCommon(ctx, vf[0], mac, status, 0x1E0u, ctx->vu0_clip_flags);

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

// PERF-V0G: shared subroutine 640..6B8 called by 348 and 4B0 (transforms vf11..vf8 by vf4..vf7 into vf15..vf12)
inline void run_sub_640(__m128 vf[32], __m128 &vacc, bool &dirtyAcc, uint32_t &stickyFlags,
                        uint32_t &mac, uint32_t &status, __m128 vzero)
{
    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4, 11,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5, 11,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6, 11,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7, 11, 15), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4, 10,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5, 10,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6, 10,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7, 10, 14), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4,  9,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5,  9,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6,  9,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7,  9, 13), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 0, 1, 0xF, 4,  8,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 1, 1, 0xF, 5,  8,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 2, 1, 0xF, 6,  8,  0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 7,  8, 12), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
}

// PERF-V0G: 348..488 (frustum/clip test with 3 early-out IBNE branches)
inline void run_348(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 4; i <= 11; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    const __m128 vzero = _mm_setzero_ps();
    uint32_t stickyFlags = 0u;
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
    uint32_t workingClip = ctx->vu0_clip_flags & 0xFFFFFFu;

    run_sub_640(vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero);

    const int32_t vi14 = 200;
    const int32_t vi15 = 0x6D;
    const int32_t vi1 = 63;
    int32_t vi2 = 0, vi3 = 0, vi4 = 0;

    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    workingClip = execClipStep(vf[1], vf[1], workingClip);
    const uint32_t clip1 = workingClip;
    workingClip = execClipStep(vf[2], vf[2], workingClip);
    const uint32_t clip2 = workingClip;
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    vi3 = static_cast<int32_t>(clip1 & 0x0FFFu);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    vi4 = static_cast<int32_t>(clip2 & 0x0FFFu);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    vi3 |= vi4;
    workingClip = execClipStep(vf[1], vf[1], workingClip);
    const uint32_t clip3 = workingClip;
    vi2 = vi3 & vi1;
    workingClip = execClipStep(vf[2], vf[2], workingClip);
    const uint32_t clip4 = workingClip;
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    if (vi2 != 0)
    {
        execFmacFast<true>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi2 = vi3 & vi1;
    }
    else
    {
        execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi3 = static_cast<int32_t>(clip3 & 0x0FFFu);
        execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi4 = static_cast<int32_t>(clip4 & 0x0FFFu);
        execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi3 |= vi4;
        workingClip = execClipStep(vf[1], vf[1], workingClip);
        const uint32_t clip5 = workingClip;
        vi2 = vi3 & vi1;
        workingClip = execClipStep(vf[2], vf[2], workingClip);
        const uint32_t clip6 = workingClip;
        execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

        if (vi2 != 0)
        {
            execFmacFast<true>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
            vi2 = vi3 & vi1;
        }
        else
        {
            execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
            execFmacFast<true> (mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
            vi3 = static_cast<int32_t>(clip5 & 0x0FFFu);
            vi4 = static_cast<int32_t>(clip6 & 0x0FFFu);
            vi3 |= vi4;
            workingClip = execClipStep(vf[1], vf[1], workingClip);
            const uint32_t clip7 = workingClip;
            vi2 = vi3 & vi1;
            workingClip = execClipStep(vf[2], vf[2], workingClip);
            const uint32_t clip8 = workingClip;

            if (vi2 != 0)
            {
                vi2 = vi3 & vi1;
            }
            else
            {
                vi3 = static_cast<int32_t>(clip7 & 0x0FFFu);
                vi4 = static_cast<int32_t>(clip8 & 0x0FFFu);
                vi3 |= vi4;
                vi2 = vi3 & vi1;
            }
        }
    }

    status |= (stickyFlags & 0xFu) << 6;
    ctx->vu0_vf[1] = vf[1];
    ctx->vu0_vf[2] = vf[2];
    for (int i = 12; i <= 15; ++i)
        ctx->vu0_vf[i] = vf[i];
    if (dirtyAcc)
        ctx->vu0_acc = vacc;

    ctx->vi[1] = static_cast<uint16_t>(vi1);
    ctx->vi[2] = static_cast<uint16_t>(vi2);
    ctx->vi[3] = static_cast<uint16_t>(vi3);
    ctx->vi[4] = static_cast<uint16_t>(vi4);
    ctx->vi[14] = static_cast<uint16_t>(vi14);
    ctx->vi[15] = static_cast<uint16_t>(vi15);
    finalizeCommon(ctx, vf[0], mac, status, 0x490u, workingClip);

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

// PERF-V0G: 4B0..5F0 (frustum/clip test with 3 early-out IBEQ branches)
inline void run_4b0(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 4; i <= 11; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    const __m128 vzero = _mm_setzero_ps();
    uint32_t stickyFlags = 0u;
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
    uint32_t workingClip = ctx->vu0_clip_flags & 0xFFFFFFu;

    run_sub_640(vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero);

    const int32_t vi14 = 200;
    const int32_t vi15 = 0x9A;
    int32_t vi2 = 63;
    int32_t vi3 = 0;

    using namespace vu0fast;
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    workingClip = execClipStep(vf[1], vf[1], workingClip);
    const uint32_t clip1 = workingClip;
    workingClip = execClipStep(vf[2], vf[2], workingClip);
    const uint32_t clip2 = workingClip;
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    vi3 = static_cast<int32_t>(clip1 & 0x0FFFu);
    execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    vi2 &= vi3;
    execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
    vi3 = static_cast<int32_t>(clip2 & 0x0FFFu);
    workingClip = execClipStep(vf[1], vf[1], workingClip);
    const uint32_t clip3 = workingClip;
    vi2 &= vi3;
    workingClip = execClipStep(vf[2], vf[2], workingClip);
    const uint32_t clip4 = workingClip;
    execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

    if (vi2 == 0)
    {
        execFmacFast<true>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi2 &= vi3;
    }
    else
    {
        execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        execFmacFast<false>(mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi3 = static_cast<int32_t>(clip3 & 0x0FFFu);
        execFmacFast<false>(mkOp(OP_MUL,  SRC_VT_BC, 3, 1, 0xF, 15, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi2 &= vi3;
        execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 14, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
        vi3 = static_cast<int32_t>(clip4 & 0x0FFFu);
        workingClip = execClipStep(vf[1], vf[1], workingClip);
        const uint32_t clip5 = workingClip;
        vi2 &= vi3;
        workingClip = execClipStep(vf[2], vf[2], workingClip);
        const uint32_t clip6 = workingClip;
        execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 1, 0xF, 13, 0, 0), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);

        if (vi2 == 0)
        {
            execFmacFast<true>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
            vi2 &= vi3;
        }
        else
        {
            execFmacFast<false>(mkOp(OP_MADD, SRC_VT_BC, 3, 0, 0xF, 12, 0, 1), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
            execFmacFast<true> (mkOp(OP_MSUB, SRC_VT_BC, 3, 0, 0xF, 12, 0, 2), vf, vacc, dirtyAcc, stickyFlags, mac, status, vzero, vzero);
            vi3 = static_cast<int32_t>(clip5 & 0x0FFFu);
            vi2 &= vi3;
            vi3 = static_cast<int32_t>(clip6 & 0x0FFFu);
            workingClip = execClipStep(vf[1], vf[1], workingClip);
            const uint32_t clip7 = workingClip;
            vi2 &= vi3;
            workingClip = execClipStep(vf[2], vf[2], workingClip);
            const uint32_t clip8 = workingClip;

            if (vi2 == 0)
            {
                vi2 &= vi3;
            }
            else
            {
                vi3 = static_cast<int32_t>(clip7 & 0x0FFFu);
                vi2 &= vi3;
                vi3 = static_cast<int32_t>(clip8 & 0x0FFFu);
                vi2 &= vi3;
            }
        }
    }

    status |= (stickyFlags & 0xFu) << 6;
    ctx->vu0_vf[1] = vf[1];
    ctx->vu0_vf[2] = vf[2];
    for (int i = 12; i <= 15; ++i)
        ctx->vu0_vf[i] = vf[i];
    if (dirtyAcc)
        ctx->vu0_acc = vacc;

    ctx->vi[2] = static_cast<uint16_t>(vi2);
    ctx->vi[3] = static_cast<uint16_t>(vi3);
    ctx->vi[14] = static_cast<uint16_t>(vi14);
    ctx->vi[15] = static_cast<uint16_t>(vi15);
    finalizeCommon(ctx, vf[0], mac, status, 0x5F8u, workingClip);

    if (changeCsr)
        _mm_setcsr(prevCsr);
}

inline FuncHandler lookupHandler(const uint8_t *vu0Code, uint64_t generation, uint32_t startPC) // PERF-V0G
{
    if (vu0fast::getCachedImageHash(vu0Code, generation) != kTargetImageHash)
        return nullptr;
    switch (startPC)
    {
    case 0x000u: return &run_000;
    case 0x088u: return &run_088;
    case 0x0F0u: return &run_0f0;
    case 0x348u: return &run_348;
    case 0x4B0u: return &run_4b0;
    case 0xCD0u: return &run_cd0;
    default:     return nullptr;
    }
}

struct FuncCheckStats // PERF-V0G
{
    std::mutex mtx;
    uint64_t calls[6]{};     // 0:cd0, 1:088, 2:4b0, 3:000, 4:348, 5:0f0
    uint64_t mismatches[6]{};
    uint32_t recorded = 0;
    vu0fast::MismatchRec first[3]{};
    std::chrono::steady_clock::time_point lastPrint{};
};

inline FuncCheckStats &funcCheckStats() // PERF-V0G
{
    static FuncCheckStats s;
    return s;
}

inline int entryIndexForPc(uint32_t startPC) // PERF-V0G
{
    switch (startPC)
    {
    case 0xCD0u: return 0;
    case 0x088u: return 1;
    case 0x4B0u: return 2;
    case 0x000u: return 3;
    case 0x348u: return 4;
    case 0x0F0u: return 5;
    default:     return -1;
    }
}

inline void compareAndRecord(uint32_t startPC, const R5900Context &funcCtx, const R5900Context &fastCtx) // PERF-V0G
{
    bool anyMismatch = false;
    char firstField[24]{};
    uint64_t firstFunc = 0;
    uint64_t firstFast = 0;

    auto cmpU64 = [&](const char *name, uint64_t hv, uint64_t fv)
    {
        if (hv != fv && !anyMismatch)
        {
            anyMismatch = true;
            std::snprintf(firstField, sizeof(firstField), "%s", name);
            firstFunc = hv;
            firstFast = fv;
        }
    };

    auto cmpM128 = [&](const char *prefix, int idx, __m128 hm, __m128 fm)
    {
        alignas(16) uint32_t hw[4]{}, fw[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(hw), _mm_castps_si128(hm));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fw), _mm_castps_si128(fm));
        static constexpr char kLanes[4] = {'x', 'y', 'z', 'w'};
        for (int l = 0; l < 4; ++l)
        {
            if (hw[l] != fw[l] && !anyMismatch)
            {
                anyMismatch = true;
                if (idx >= 0)
                    std::snprintf(firstField, sizeof(firstField), "%s%d.%c", prefix, idx, kLanes[l]);
                else
                    std::snprintf(firstField, sizeof(firstField), "%s.%c", prefix, kLanes[l]);
                firstFunc = hw[l];
                firstFast = fw[l];
            }
        }
    };

    auto cmpF32 = [&](const char *name, float hf, float ff)
    {
        uint32_t hb = 0, fb = 0;
        std::memcpy(&hb, &hf, 4);
        std::memcpy(&fb, &ff, 4);
        cmpU64(name, hb, fb);
    };

    for (int i = 0; i < 32; ++i)
        cmpM128("vf", i, funcCtx.vu0_vf[i], fastCtx.vu0_vf[i]);
    for (int i = 0; i < 16; ++i)
    {
        char n[16];
        std::snprintf(n, sizeof(n), "vi%d", i);
        cmpU64(n, funcCtx.vi[i], fastCtx.vi[i]);
    }
    cmpM128("acc", -1, funcCtx.vu0_acc, fastCtx.vu0_acc);
    cmpF32("q", funcCtx.vu0_q, fastCtx.vu0_q);
    cmpF32("p", funcCtx.vu0_p, fastCtx.vu0_p);
    cmpF32("i", funcCtx.vu0_i, fastCtx.vu0_i);
    cmpM128("r", -1, funcCtx.vu0_r, fastCtx.vu0_r);
    cmpU64("mac", funcCtx.vu0_mac_flags, fastCtx.vu0_mac_flags);
    cmpU64("status", funcCtx.vu0_status, fastCtx.vu0_status);
    cmpU64("clip", funcCtx.vu0_clip_flags, fastCtx.vu0_clip_flags);
    cmpU64("clip2", funcCtx.vu0_clip_flags2, fastCtx.vu0_clip_flags2);
    cmpU64("pc", funcCtx.vu0_pc, fastCtx.vu0_pc);
    cmpU64("tpc", funcCtx.vu0_tpc, fastCtx.vu0_tpc);
    cmpU64("vpu_stat", funcCtx.vu0_vpu_stat, fastCtx.vu0_vpu_stat);
    cmpU64("vpu_stat2", funcCtx.vu0_vpu_stat2, fastCtx.vu0_vpu_stat2);
    cmpU64("itop", funcCtx.vu0_itop, fastCtx.vu0_itop);

    const int eidx = entryIndexForPc(startPC);
    if (eidx < 0)
        return;

    FuncCheckStats &cs = funcCheckStats();
    std::lock_guard<std::mutex> lock(cs.mtx);
    cs.calls[eidx]++;
    if (anyMismatch)
    {
        cs.mismatches[eidx]++;
        if (cs.recorded < 3u)
        {
            vu0fast::MismatchRec &rec = cs.first[cs.recorded++];
            rec.pc = startPC;
            std::snprintf(rec.field, sizeof(rec.field), "%s", firstField);
            rec.fastVal = firstFunc;
            rec.normVal = firstFast;
        }
    }
}

inline void maybePrintCheckStats() // PERF-V0G
{
    FuncCheckStats &cs = funcCheckStats();
    std::lock_guard<std::mutex> lock(cs.mtx);
    const auto now = std::chrono::steady_clock::now();
    if (cs.lastPrint == std::chrono::steady_clock::time_point{})
    {
        cs.lastPrint = now;
        return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - cs.lastPrint).count() < 5)
        return;
    cs.lastPrint = now;
    std::printf("[v0func] cd0=%llu/%llu 088=%llu/%llu 4b0=%llu/%llu 000=%llu/%llu 348=%llu/%llu 0f0=%llu/%llu",
                static_cast<unsigned long long>(cs.calls[0]), static_cast<unsigned long long>(cs.mismatches[0]),
                static_cast<unsigned long long>(cs.calls[1]), static_cast<unsigned long long>(cs.mismatches[1]),
                static_cast<unsigned long long>(cs.calls[2]), static_cast<unsigned long long>(cs.mismatches[2]),
                static_cast<unsigned long long>(cs.calls[3]), static_cast<unsigned long long>(cs.mismatches[3]),
                static_cast<unsigned long long>(cs.calls[4]), static_cast<unsigned long long>(cs.mismatches[4]),
                static_cast<unsigned long long>(cs.calls[5]), static_cast<unsigned long long>(cs.mismatches[5]));
    if (cs.recorded > 0u)
    {
        std::printf(" first:");
        for (uint32_t i = 0; i < cs.recorded; ++i)
        {
            std::printf(" pc=%03x field=%s(h=0x%llx,f=0x%llx)",
                        cs.first[i].pc,
                        cs.first[i].field,
                        static_cast<unsigned long long>(cs.first[i].fastVal),
                        static_cast<unsigned long long>(cs.first[i].normVal));
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}
} // namespace vu0func
