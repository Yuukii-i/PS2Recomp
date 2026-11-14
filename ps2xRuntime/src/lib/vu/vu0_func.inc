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
        return e && e[0] == '1';
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

// PERF-V0G: 000..080 (4x4 transform of vf8..vf11 by vf4..vf7)
inline void run_000(R5900Context *ctx)
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

    uint32_t stickyFlags = 0u;
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
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

// PERF-V0G: 088..0E8 (scale vf4,vf6 by vf3.x + matrix-vec multiply into vf5,vf7)
inline void run_088(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 3; i <= 14; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    uint32_t stickyFlags = 0u;
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
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

// PERF-V0G: CD0..D30 (scale vf1,vf15 by vf3.x + matrix-vec multiply into vf2,vf16)
inline void run_cd0(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    vf[1] = ctx->vu0_vf[1];
    vf[2] = ctx->vu0_vf[2];
    vf[3] = ctx->vu0_vf[3];
    for (int i = 8; i <= 16; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    uint32_t stickyFlags = 0u;
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
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

// PERF-V0G: 0F0..1D8 (4x4 transform + RSQRT normalization of vf8..vf10 into vf12..vf14)
inline void run_0f0(R5900Context *ctx)
{
    const uint32_t prevCsr = _mm_getcsr();
    const bool changeCsr = (prevCsr & 0x6000u) != 0x6000u;
    if (changeCsr)
        _mm_setcsr(prevCsr | 0x6000u);

    __m128 vf[32];
    vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 1; i <= 14; ++i)
        vf[i] = ctx->vu0_vf[i];
    __m128 vacc = ctx->vu0_acc;
    bool dirtyAcc = false;

    float q = ctx->vu0_q;
    __m128 vq = _mm_set1_ps(vu0fast::normalizeOperand(q));
    const __m128 vzero = _mm_setzero_ps();
    uint32_t stickyFlags = 0u;
    uint32_t mac = ctx->vu0_mac_flags;
    uint32_t status = ctx->vu0_status;
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
