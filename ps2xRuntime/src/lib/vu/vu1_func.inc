// vu1_func.inc -- Task F1: functional VU1 engine for microcode image 86503d3905e35276 (PS2X_VU1_FUNC=1, default OFF).
// Included by ps2_vu1_core.cpp right before VU1Interpreter::execute (needs s_vfyCapture, GifPathId, PS2Memory, GS). No header is touched.
// Program output (VU memory + GIF bytes) is a pure function of VU memory at MSCAL (+ startPC/TOP/ITOP), so only memory writes and GIF packets are reproduced;
// cycles, flags and exit registers are not. The dispatcher (0008..0040) and the handlers 0000 0050 0060 0070 0088 0C80 00A8 are evaluated in closed form;
// any other handler / malformed GIF tag / unexpected state restores the VU memory snapshot and returns false (the caller then runs the exact interpreter).
// Float math: the FMAC primitive below is a value-only mirror of lbFmac / vuhot::fmacSlow (vu1_native_hand.inc / vu1_hot_ops.h): operands normalised
// (N4), fused single-rounding multiply-add in float, and the exact-double re-classification (zero / underflow -> signed 0, overflow -> signed FLT_MAX)
// whenever a lane of the result or of the product leaves the exponent window [2,253]. (vu1_hot_ops.h itself cannot be included in this TU: its fmacR
// touches private VU1Interpreter members and needs `#define private public`.)
#include <cfloat>
#include <unordered_map>
#include <utility>

extern void ps2xSafeLog(const char *fmt, ...);

namespace vu1func
{
constexpr uint64_t kImageHash = 0x86503d3905e35276ull; // FNV-1a of the 16 KB VU1 code image == s_hcCurHash (set by rebuildDecodedCodeCache)
constexpr uint32_t kMaxKickBytes = 0x10000u;           // == XgkickPipeline::kBufferSize

inline __m128 norm4(__m128 v) // vuhot::N4 (normalizeOperand on 4 lanes)
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

inline uint32_t outOfWindow(__m128 v) // bit c set: lane c exponent outside [2,253]
{
    const __m128i ev = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(v), 23), _mm_set1_epi32(0xFF));
    return static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_cmplt_epi32(ev, _mm_set1_epi32(2)), _mm_cmpgt_epi32(ev, _mm_set1_epi32(253))))));
}

// KIND: 0 = ADD (a+b), 1 = SUB (a-b), 2 = MADD (ac+a*b), 3 = MSUB (ac-a*b), 4 = MUL (a*b). All four lanes (DEST = xyzw).
template <int KIND>
inline __m128 fmacV(__m128 a, __m128 b, __m128 ac)
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    __m128 ac4 = _mm_setzero_ps(), val, prodf = _mm_setzero_ps();
    uint32_t bad;
    // PERF-F2Z: lanes with an exactly-zero operand are exact in the fast path
    const __m128i absMask = _mm_set1_epi32(0x7FFFFFFF);
    const __m128i za = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(a4), absMask), _mm_setzero_si128());
    const __m128i zb = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(b4), absMask), _mm_setzero_si128());
    const uint32_t zmask = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(za, zb))));
    if constexpr (KIND == 0 || KIND == 1)
    {
        val = (KIND == 0) ? _mm_add_ps(a4, b4) : _mm_sub_ps(a4, b4);
        const uint32_t bothZ = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(za, zb))));
        bad = outOfWindow(val) & ~bothZ;
    }
    else if constexpr (KIND == 4)
    {
        val = _mm_mul_ps(a4, b4);
        bad = outOfWindow(val) & ~zmask;
    }
    else
    {
        ac4 = norm4(ac);
        val = (KIND == 2) ? _mm_fmadd_ps(a4, b4, ac4) : _mm_fnmadd_ps(a4, b4, ac4);
        prodf = _mm_mul_ps(a4, b4);
        const __m128i acz = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(ac4), absMask), _mm_setzero_si128());
        const uint32_t aczm = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(acz)));
        const uint32_t acin = ~outOfWindow(ac4) & 0xFu;
        bad = (outOfWindow(val) & ~(zmask & (aczm | acin))) | (outOfWindow(prodf) & ~zmask);
    }
    if (bad == 0u)
        return val;
    alignas(16) float A[4], B[4], C[4], V[4];
    _mm_store_ps(A, a4);
    _mm_store_ps(B, b4);
    _mm_store_ps(C, ac4);
    _mm_store_ps(V, val);
    alignas(16) uint32_t out[4];
    for (int l = 0; l < 4; ++l)
    {
        const double ad = static_cast<double>(A[l]), bd = static_cast<double>(B[l]);
        double ex;
        if constexpr (KIND == 0)
            ex = ad + bd;
        else if constexpr (KIND == 1)
            ex = ad - bd;
        else if constexpr (KIND == 2)
            ex = static_cast<double>(C[l]) + ad * bd;
        else if constexpr (KIND == 3)
            ex = static_cast<double>(C[l]) - ad * bd;
        else
            ex = ad * bd;
        const double mag = std::fabs(ex);
        const bool zero = mag == 0.0;
        const bool over = mag > static_cast<double>(FLT_MAX);
        const bool under = (mag < static_cast<double>(FLT_MIN)) && !zero;
        uint64_t exb; // PERF-F2Z
        std::memcpy(&exb, &ex, 8);
        const uint32_t sg = static_cast<uint32_t>(exb >> 63) << 31;
        if (zero || under)
            out[l] = sg;
        else if (over)
            out[l] = sg | 0x7F7FFFFFu;
        else
            std::memcpy(&out[l], &V[l], 4);
    }
    return _mm_load_ps(reinterpret_cast<const float *>(out));
}

template <int L>
inline __m128 bc(__m128 v) // broadcast lane L (operand normalisation happens in fmacV)
{
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(L, L, L, L));
}

struct Regs
{
    __m128 vf[32];
    __m128 acc;
    uint16_t vi[16];
};

inline __m128 ldq(const uint8_t *vu, uint32_t q) { return _mm_loadu_ps(reinterpret_cast<const float *>(vu + ((q & 0x3FFu) << 4))); }
struct UndoEnt
{
    uint16_t qword;
    uint8_t old[16];
};
inline std::vector<UndoEnt> &undoLog()
{
    static thread_local std::vector<UndoEnt> log = []() { std::vector<UndoEnt> v; v.reserve(2048); return v; }();
    return log;
}
// The ONLY function that writes VU memory inside the engine: logs the old qword first (undo log replaces the 16 KB snapshot).
inline void stq(uint8_t *vu, uint32_t q, __m128 v)
{
    const uint32_t qi = q & 0x3FFu;
    UndoEnt e;
    e.qword = static_cast<uint16_t>(qi);
    std::memcpy(e.old, vu + (qi << 4), 16);
    undoLog().push_back(e);
    _mm_storeu_ps(reinterpret_cast<float *>(vu + (qi << 4)), v);
}
// PERF: bulk-log `count` qwords starting at q (one resize + memcpys instead of a push_back per store); the stores into that range may then use stqRaw.
inline void logRange(const uint8_t *vu, uint32_t q, uint32_t count)
{
    std::vector<UndoEnt> &log = undoLog();
    const size_t base = log.size();
    log.resize(base + count);
    UndoEnt *e = log.data() + base;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t qi = (q + i) & 0x3FFu;
        e[i].qword = static_cast<uint16_t>(qi);
        std::memcpy(e[i].old, vu + (qi << 4), 16);
    }
}
inline void stqRaw(uint8_t *vu, uint32_t q, __m128 v) { _mm_storeu_ps(reinterpret_cast<float *>(vu + ((q & 0x3FFu) << 4)), v); } // range already logged
inline uint16_t ilw(const uint8_t *vu, uint32_t q, uint32_t f) // ILW.f: low 16 bits of word f
{
    uint32_t w;
    std::memcpy(&w, vu + ((q & 0x3FFu) << 4) + 4u * f, 4);
    return static_cast<uint16_t>(w & 0xFFFFu);
}

// Handler 00A8: matrix setup (00A8..0150) + table transform loop (0158..0180), docs/VU1_HANDLER_FORMS.md sections 1, 2 (verified bit-exact there).
// Loop memory order inside one pass: SQI vf31 (0158), LQ.xy (0160), SQI vf6 (0168), LQI.zw (0170); the vf5 that pass k multiplies holds the xy/zw
// loaded by pass k-1 (pass 1: the LQI of 0138), and the vf6 stored by pass k is the MADDw result of pass k-1 (pass 1: the prologue MADDw at 0150).
static bool handler00A8(Regs &R, uint8_t *vu)
{
    const __m128 zero = _mm_setzero_ps();
    __m128 vf3 = ldq(vu, 980), vf1 = ldq(vu, 998), vf5 = ldq(vu, 999), vf7 = ldq(vu, 1000), vf4 = ldq(vu, 981), vf6 = ldq(vu, 1001);
    R.vf[31] = zero; // SUB.xyzw vf31, vf0, vf0
    __m128 acc = fmacV<4>(vf1, bc<0>(vf3), zero);
    acc = fmacV<2>(vf5, bc<1>(vf3), acc);
    vf3 = fmacV<2>(vf7, bc<2>(vf3), acc); // R2
    const uint16_t vi3 = ilw(vu, 1011, 0), vi2 = ilw(vu, 1011, 3);
    acc = fmacV<4>(vf1, bc<0>(vf4), zero);
    acc = fmacV<2>(vf5, bc<1>(vf4), acc);
    acc = fmacV<2>(vf7, bc<2>(vf4), acc);
    vf4 = fmacV<2>(vf6, bc<3>(vf4), acc); // R3
    __m128 vf2 = ldq(vu, 979);
    const __m128 vf8 = ldq(vu, 978);
    acc = fmacV<4>(vf1, bc<0>(vf2), zero);
    acc = fmacV<2>(vf5, bc<1>(vf2), acc);
    vf2 = fmacV<2>(vf7, bc<2>(vf2), acc); // R1
    acc = fmacV<4>(vf1, bc<0>(vf8), zero);
    acc = fmacV<2>(vf5, bc<1>(vf8), acc);
    vf1 = fmacV<2>(vf7, bc<2>(vf8), acc); // R0
    uint16_t vi5 = static_cast<uint16_t>(vi3 + 636u), vi1 = static_cast<uint16_t>(vi3 + 476u);
    const __m128 vf25 = ldq(vu, vi1++);
    vf5 = ldq(vu, vi1++);
    acc = fmacV<4>(vf1, bc<0>(vf25), zero);
    acc = fmacV<2>(vf2, bc<1>(vf25), acc);
    acc = fmacV<2>(vf3, bc<2>(vf25), acc);
    vf6 = fmacV<2>(vf4, bc<3>(vf25), acc);
    const uint16_t vi6 = static_cast<uint16_t>(vi5 + vi2);
    uint16_t vi7 = static_cast<uint16_t>(vi3 + 716u);
    if (vi2 == 0u || vi2 > 512u) // 0 would be 65536 passes; anything this large is not a real table transform
        return false;
    do
    {
        const __m128 cur = vf5;
        stq(vu, vi7++, R.vf[31]);                         // 0158 SQI vf31
        acc = fmacV<4>(vf1, bc<0>(cur), zero);            // 0158 MULAx
        acc = fmacV<2>(vf2, bc<1>(cur), acc);             // 0160 MADDAy
        const __m128 xy = ldq(vu, vi1);                   // 0160 LQ.xy vf5, 0(vi1)
        acc = fmacV<2>(vf3, bc<2>(cur), acc);             // 0168 MADDAz
        stq(vu, vi5++, vf6);                              // 0168 SQI vf6 (value of the previous pass)
        const __m128 nv6 = fmacV<2>(vf4, bc<3>(cur), acc); // 0170 MADDw vf6
        const __m128 zw = ldq(vu, vi1++);                 // 0170 LQI.zw vf5
        vf6 = nv6;
        vf5 = _mm_shuffle_ps(xy, zw, _MM_SHUFFLE(3, 2, 1, 0)); // x,y from xy ; z,w from zw
    } while (vi5 != vi6);                                 // 0178 IBNE vi6, vi5
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3; R.vf[4] = vf4; R.vf[5] = vf5; R.vf[6] = vf6; R.vf[7] = vf7; R.vf[8] = vf8; R.vf[25] = vf25;
    R.acc = acc;
    R.vi[1] = vi1; R.vi[2] = vi2; R.vi[3] = vi3; R.vi[5] = vi5; R.vi[6] = vi6; R.vi[7] = vi7;
    return true;
}

static uint64_t s_taken = 0, s_fallback = 0;
static std::unordered_map<uint32_t, uint64_t> s_startPcs; // startPC of 0xF001 fallbacks -> count
static std::unordered_map<uint32_t, uint64_t> s_fbPcs; // handler pc (or 0xF000 + reason) -> count
static uint32_t s_fbPc = 0;

// 1EE0 fallback reasons (counted just before returning false; printed in the periodic [vfunc] line as 1ee0_why)
enum Fb1EE0Reason : int { kFbRigidN = 0, kFbSkinN, kFbSkinAlias13, kFbSkinGatherAlias, kFbSkinDivZero, kFbSkinKick, kFbRigidClip, kFbRigidKick, kFbClipKick, kFbCount };
static const char *const kFb1EE0Names[kFbCount] = {"rigidN_or_vi13", "skinN_range", "skin_alias_1012_1013_1023_vi13", "skin_gather_alias", "skin_div_zero", "skin_kick", "rigid_clipPolygon", "rigid_kick", "clip_kick"};
static uint64_t s_fb1EE0[kFbCount] = {};
static constexpr uint32_t kMaxSkinN = 255;        // 4N+1 output qwords must fit in 1024
static uint64_t s_skinNHist[7] = {};              // N hitting the skinN guard: 0,1,2,3,<100,100..199,200+
static bool fb1EE0(int reason)
{
    ++s_fb1EE0[reason];
    return false;
}

static bool kick(const uint8_t *vu, uint32_t addr, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    const uint32_t q0 = addr & 0x3FFu;
    uint32_t total = 0;
    for (;;)
    {
        uint64_t tagLo;
        std::memcpy(&tagLo, vu + (((q0 + total / 16u) & 0x3FFu) << 4), 8);
        const uint64_t nloop = tagLo & 0x7FFFu;
        const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint64_t nreg = (tagLo >> 60) & 0xFu;
        if (nreg == 0u)
            nreg = 16u;
        uint64_t tagBytes = 16u;
        if (format == 0u)
            tagBytes += nloop * nreg * 16u;
        else if (format == 1u)
            tagBytes += ((nloop * nreg + 1u) & ~1ull) * 8u;
        else if (format == 2u)
            tagBytes += nloop * 16u;
        else
        {
            s_fbPc = 0xF010u;
            return false;
        }
        if ((tagBytes & 15u) != 0u || tagBytes > kMaxKickBytes - total)
        {
            s_fbPc = 0xF011u;
            return false;
        }
        total += static_cast<uint32_t>(tagBytes);
        if (((tagLo >> 15) & 1u) != 0u) // EOP
            break;
    }
    const uint32_t start = static_cast<uint32_t>(gif.size());
    gif.resize(start + total);
    for (uint32_t i = 0; i < total / 16u; ++i)
        std::memcpy(gif.data() + start + i * 16u, vu + (((q0 + i) & 0x3FFu) << 4), 16);
    kicks.emplace_back(start, total);
    return true;
}

// ---- Task F2: handler 1EE0 (mesh transform: skinned path 1FE0..21F8, rigid path 2440.. below) ----
// Value helpers mirroring vuhot::N / normResult / lbClamp / lbFtoi (vu1_hot_ops.h, vu1_native_hand.inc), which cannot be included in this TU.
inline float nrm1(float v)
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t e = (b >> 23) & 0xFFu;
    if (e == 0u)
        b &= 0x80000000u;
    else if (e == 0xFFu)
        b = (b & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&v, &b, 4);
    return v;
}
inline float normRes(float v) // vuhot::normResult
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t sign = b & 0x80000000u, mag = b & 0x7FFFFFFFu, e = (b >> 23) & 0xFFu;
    if (mag != 0u)
    {
        if (e == 0u)
            b = sign;
        else if (e == 0xFFu)
            b = sign | 0x7F7FFFFFu;
    }
    std::memcpy(&v, &b, 4);
    return v;
}
inline __m128 clampI(__m128 x) // lbClamp with I = 255.0f: MAXx.xyzw (vf0.x = 0) then MINIi.xyzw
{
    return _mm_min_ps(_mm_max_ps(norm4(x), _mm_setzero_ps()), _mm_set1_ps(255.0f));
}
template <int SCALE>
inline __m128i ftoiS(__m128 a) // lbFtoi
{
    const __m128 sc = _mm_mul_ps(norm4(a), _mm_set1_ps(static_cast<float>(SCALE)));
    const __m128i t = _mm_cvttps_epi32(sc);
    const __m128 ge = _mm_cmpge_ps(sc, _mm_set1_ps(2147483648.0f));
    return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(t), _mm_castsi128_ps(_mm_set1_epi32(INT32_MAX)), ge));
}
inline uint32_t ldw(const uint8_t *vu, uint32_t q, uint32_t lane) // raw 32-bit word
{
    uint32_t w;
    std::memcpy(&w, vu + ((q & 0x3FFu) << 4) + 4u * lane, 4);
    return w;
}
inline __m128i ldqi(const uint8_t *vu, uint32_t q) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu + ((q & 0x3FFu) << 4))); }
inline void stqi(uint8_t *vu, uint32_t q, __m128i v) { stq(vu, q, _mm_castsi128_ps(v)); }

inline uint32_t clipBitsV(__m128 v) // lbClipBits (CLIP.xyz against |w|)
{
    uint32_t w[4];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(w), _mm_castps_si128(v));
    const int32_t limit = (w[3] & 0x7F800000u) != 0u ? static_cast<int32_t>(w[3] & 0x7FFFFFFFu) : 0x007FFFFF;
    uint32_t fl = 0u;
    for (int k = 0; k < 3; ++k)
    {
        const int32_t o0 = static_cast<int32_t>(w[k]), o1 = static_cast<int32_t>(w[k] ^ 0x80000000u);
        if (o0 > limit)
            fl |= 1u << (2 * k);
        if (o1 > limit)
            fl |= 1u << (2 * k + 1);
    }
    return fl;
}
inline float divQ(__m128 t) // lbDivQ without the DI flag (flags are not reproduced): DIV Q, vf0.w, t.w
{
    const float den = nrm1(_mm_cvtss_f32(bc<3>(t)));
    float q;
    if (den == 0.0f)
        q = std::signbit(den) ? -FLT_MAX : FLT_MAX;
    else
        q = normRes(1.0f / den);
    return normRes(q);
}
inline float divQ2(float numRaw, float denRaw) // mirrors VU1Interpreter::execLower case 0x38 (DIV)
{
    const float num = nrm1(numRaw), den = nrm1(denRaw);
    float q;
    if (den == 0.0f)
        q = (std::signbit(num) != std::signbit(den)) ? -FLT_MAX : FLT_MAX;
    else
        q = num / den;
    return normRes(q);
}
inline __m128 maxV(__m128 a, __m128 b) // mirrors VU1Interpreter::execUpper MAX/MAXbc/MAXi
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    return _mm_blendv_ps(b4, a4, _mm_cmpgt_ps(a4, b4));
}
inline __m128 miniV(__m128 a, __m128 b) // mirrors VU1Interpreter::execUpper MINI/MINIbc/MINIi
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    return _mm_blendv_ps(b4, a4, _mm_cmplt_ps(a4, b4));
}
inline __m128 absV(__m128 v) // mirrors VU1Interpreter::execUpper case 0x1D (ABS)
{
    return _mm_and_ps(norm4(v), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
}
inline __m128 mr32(__m128 v) // mirrors VU1Interpreter::execLower case 0x31 (MR32: xyzw -> yzwx)
{
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1));
}

// Task F4: Sutherland-Hodgman 6-plane frustum clipper (2900..2F68), called from rigid2440 when no trivial-reject FCOR mask matches.
// Reproduces every context/spill/buffer/GIF store to VU memory (1020..1022, 918..959, 876..) and kicks 1023 + 876; returns to 24F8/2720 via JR vi12.
static bool clipPolygon(uint8_t *vu, uint16_t vi13, int32_t vi5, int32_t vi6, int32_t vi7, int32_t vi8, int32_t vi9, int32_t vi10, bool copyS,
                        __m128 c1v, __m128 c2v, __m128 c5v, __m128 vf28, __m128 vf29, __m128 vf10, __m128 vf30, __m128 vf31, __m128 vf7,
                        __m128 vf11, __m128 vf12, __m128 vf13, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    const __m128 zero = _mm_setzero_ps(), vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    const uint16_t retVi12 = static_cast<uint16_t>((copyS ? 0x2720u : 0x24F8u) >> 3);

    // 2900..2960: save integer context to 1020..1022
    stqi(vu, 1022u, _mm_set_epi32(vi8 & 0xFFFF, vi7 & 0xFFFF, vi6 & 0xFFFF, vi5 & 0xFFFF));
    stqi(vu, 1021u, _mm_set_epi32(retVi12, 0x7FFF, vi10 & 0xFFFF, vi9 & 0xFFFF));
    stqi(vu, 1020u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1020u), vi13, 0), 5, 1));
    const uint32_t clipMode = ldw(vu, 1012u, 1u) & 1u;

    // 2968..29E8: spill triangle vertices + wrap-around vertex 0 into 918..929
    vf13 = maxV(vf13, zero); // 2978 MAXx.xyzw vf13, vf13, vf0
    stq(vu, 918u, vf28);
    stq(vu, 921u, vf29);
    stq(vu, 924u, vf10);
    stq(vu, 927u, vf28);
    stq(vu, 920u, vf30);
    stq(vu, 923u, vf31);
    stq(vu, 926u, vf7);
    stq(vu, 925u, vf13);
    stq(vu, 929u, vf30); // 29B0 delay slot of IBEQ
    if (clipMode != 0u)
    {
        stq(vu, 919u, vf13);
        stq(vu, 922u, vf13);
        stq(vu, 928u, vf13);
    }
    else
    {
        stq(vu, 919u, vf11);
        stq(vu, 922u, vf12);
        stq(vu, 928u, vf11);
    }

    // 29F0..2C70: 6 frustum planes (-Z, +Z, -X, +X, -Y, +Y), ping-ponging 918 <-> 948
    struct PlaneSpec
    {
        float sgn;
        uint32_t mask0, mask1;
        int axis;
        uint32_t inBase, outBase;
    };
    static constexpr PlaneSpec kPlanes[6] = {
        {-1.0f, 2048u >> 6, 32u, 2, 918u, 948u},
        {1.0f, 1024u >> 6, 16u, 2, 948u, 918u},
        {-1.0f, 128u >> 6, 2u, 0, 918u, 948u},
        {1.0f, 64u >> 6, 1u, 0, 948u, 918u},
        {-1.0f, 512u >> 6, 8u, 1, 918u, 948u},
        {1.0f, 256u >> 6, 4u, 1, 948u, 918u},
    };

    int32_t polyCount = 3;
    __m128 vf19_last = zero;
    for (int p = 0; p < 6; ++p)
    {
        const PlaneSpec &pl = kPlanes[p];
        const __m128 sgnV = _mm_set1_ps(pl.sgn);
        __m128 vf17 = ldq(vu, pl.inBase + 0u);
        __m128 vf18 = ldq(vu, pl.inBase + 1u);
        __m128 vf23 = ldq(vu, pl.inBase + 2u);
        __m128 vf21 = ldq(vu, pl.inBase + 3u);
        uint32_t inPtr = pl.inBase + 4u;
        __m128 vf22 = ldq(vu, inPtr++);
        uint32_t outPtr = pl.outBase;
        int32_t outCount = 0;

        for (int32_t e = 0; e < polyCount; ++e)
        {
            const __m128 vf19 = fmacV<0>(vf23, zero, zero);
            const bool out0 = (clipBitsV(vf17) & pl.mask0) != 0u;
            const bool out1 = (clipBitsV(vf21) & pl.mask1) != 0u;
            const __m128 vf25w = fmacV<4>(vf17, sgnV, zero);
            const __m128 vf26w = fmacV<4>(vf21, sgnV, zero);
            vf23 = ldq(vu, inPtr++);

            if (!out0)
            {
                stq(vu, outPtr++, vf17);
                stq(vu, outPtr++, vf18);
                stq(vu, outPtr++, vf19);
                ++outCount;
            }
            if (out0 != out1)
            {
                // 2EC8..2F68: edge intersection interpolator
                __m128 d0v = fmacV<1>(vf17, bc<3>(vf25w), zero);
                __m128 d1v = fmacV<1>(vf21, bc<3>(vf26w), zero);
                for (int r = 0; r < pl.axis; ++r)
                {
                    d0v = mr32(d0v);
                    d1v = mr32(d1v);
                }
                const __m128 d1n = fmacV<0>(zero, d1v, zero); // 2F00 ADDx.y vf24, vf0, vf26
                const float den = _mm_cvtss_f32(fmacV<1>(d1v, d0v, zero));
                const float q = divQ2(1.0f, den);
                const float absD0 = _mm_cvtss_f32(absV(d0v));
                const float absD1 = _mm_cvtss_f32(absV(d1n));
                const float absQ = _mm_cvtss_f32(absV(_mm_set1_ps(normRes(q))));
                const __m128 bD0 = _mm_set1_ps(absD0), bD1 = _mm_set1_ps(absD1), bQ = _mm_set1_ps(absQ);

                const __m128 pos = fmacV<4>(fmacV<2>(vf21, bD0, fmacV<4>(vf17, bD1, zero)), bQ, zero);
                const __m128 col = fmacV<0>(fmacV<4>(fmacV<2>(vf22, bD0, fmacV<4>(vf18, bD1, zero)), bQ, zero), _mm_set1_ps(0.001f), zero);
                const __m128 st = fmacV<4>(fmacV<2>(vf23, bD0, fmacV<4>(vf19, bD1, zero)), bQ, zero);
                const __m128 m1012 = ldq(vu, 1012u); // 2F18 + 2F40: SQ.yzw m1012, 876(vi0)
                stq(vu, 876u, _mm_blend_ps(ldq(vu, 876u), m1012, 0xE));

                stq(vu, outPtr++, pos);
                stq(vu, outPtr++, col);
                stq(vu, outPtr++, st);
                ++outCount;
            }
            vf17 = fmacV<0>(vf21, zero, zero);
            vf18 = fmacV<0>(vf22, zero, zero);
            vf21 = ldq(vu, inPtr++);
            if (e + 1 < polyCount)
                vf22 = ldq(vu, inPtr++);
        }

        if (outCount == 0)
            return true; // 2E88 -> 2D50..2DA8: completely clipped out

        // 2E90..2EC0: wrap-around copy of vertex 0 + screen-space transform of v0 on last plane
        const __m128 w0 = ldq(vu, pl.outBase + 0u);
        const __m128 w1 = ldq(vu, pl.outBase + 1u);
        const __m128 w2 = ldq(vu, pl.outBase + 2u);
        stq(vu, outPtr++, w0);
        stq(vu, outPtr++, w1);
        stq(vu, outPtr++, w2);
        if (p == 5)
            vf19_last = fmacV<2>(c2v, bc<3>(w0), fmacV<4>(c1v, w0, zero));
        polyCount = outCount;
    }

    // 2C78..2D48: emit clipped polygon GIF packet at 876..
    const uint32_t tagX = (static_cast<uint32_t>(polyCount) + 32768u) & 0xFFFFu;
    stqi(vu, 876u, _mm_insert_epi32(ldqi(vu, 876u), static_cast<int32_t>(tagX), 0));
    if (!kick(vu, 1023u, kicks, gif))
        return fb1EE0(kFbClipKick);

    float q = divQ2(1.0f, _mm_cvtss_f32(bc<3>(vf19_last)));
    uint32_t ptr = 918u;
    uint32_t outQ = 877u;
    __m128 vf25 = ldq(vu, ptr + 3u);
    for (int32_t k = 0; k < polyCount; ++k)
    {
        const __m128 qb = _mm_set1_ps(q);
        const __m128 f2 = fmacV<4>(vf0, bc<2>(c5v), zero);
        __m128 vf8 = fmacV<2>(bc<3>(c5v), qb, f2);
        const __m128 vf26 = ldq(vu, ptr + 1u);
        const __m128 vf21 = fmacV<4>(vf19_last, qb, zero);
        __m128 vf27 = ldq(vu, ptr + 2u);
        vf19_last = fmacV<2>(c2v, bc<3>(vf25), fmacV<4>(c1v, vf25, zero));
        vf8 = maxV(vf8, zero);
        vf27 = _mm_blend_ps(vf27, fmacV<4>(vf27, qb, zero), 0x7);
        vf8 = miniV(vf8, _mm_set1_ps(255.0f));
        const __m128i rg = ftoiS<1>(vf26);
        const __m128i xy = ftoiS<16>(vf21);
        q = divQ2(1.0f, _mm_cvtss_f32(bc<3>(vf19_last)));
        ptr += 3u;
        vf25 = ldq(vu, ptr + 3u);
        const __m128i fw = ftoiS<16>(vf8);
        stq(vu, outQ + 1u, vf27);
        stqi(vu, outQ + 2u, rg);
        stqi(vu, outQ + 3u, xy);
        stqi(vu, outQ + 0u, _mm_insert_epi32(ldqi(vu, outQ + 0u), _mm_extract_epi32(fw, 3), 3));
        outQ += 4u;
    }
    return kick(vu, 876u, kicks, gif);
}

// Rigid path 2440 (per-vertex transform / trivial-reject / assemble, loop 24C8..25B0): 1:1 value + memory port of lbVert (vu1_native_hand.inc), which is exact on every
// record without real clipping. Differences: runs in place on VU memory through stq (same program order of loads/stores as lbVert's private copy, so the data
// seen by later loads is identical); flags / STATUS / cycles / exit registers dropped; the epilogue 25B8..2200 (1023 swap, XGKICK vi7, XTOP) added.
// Real clipping (no mask of the 6 trivial-reject tests matches, routine 2900+) is handled by clipPolygon(...).
static bool rigid2440(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t top, int32_t N, uint32_t i4base, uint32_t s2base, uint32_t s3base, uint32_t ubase, uint32_t vf27x, uint32_t vf27y,
                      bool copyS, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    constexpr int kMaxN = 300;
    const __m128 zero = _mm_setzero_ps(), vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), v2048 = _mm_set1_ps(2048.0f);
    const __m128 c1v = ldq(vu, 1002), c2v = ldq(vu, 1003), c5v = ldq(vu, 1016), c14v = ldq(vu, 1014), vf16 = ldq(vu, 1013);
    const uint32_t m1012z = ldw(vu, 1012, 2), m1012w = ldw(vu, 1012, 3);
    const auto V7 = [&](__m128 s) { return _mm_blend_ps(s, v2048, 8); };
    static thread_local __m128 Ps[kMaxN + 2], As[kMaxN + 2], Ss[kMaxN + 2], Cs[kMaxN + 2], Tv[kMaxN + 2], Lit[kMaxN + 2];
    static thread_local float Qf[kMaxN + 2];
    static thread_local uint32_t Fb[kMaxN + 2];
    static thread_local int32_t Wd[kMaxN + 2];
    const auto s16 = [](int32_t v) { return static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(v))); };

    int32_t vi1 = 0, vi5 = N, vi6 = static_cast<int32_t>(ubase), vi8 = -2, vi9 = -2, vi10 = 0, vi4 = static_cast<int32_t>(i4base);
    int32_t vi3 = static_cast<int32_t>(s3base), pS = static_cast<int32_t>(s2base);
    const int32_t vi4_0 = vi4;
    Wd[0] = s16(static_cast<int32_t>(ldw(vu, static_cast<uint32_t>(vi4_0), 0) & 0xFFFFu)); // ILW.x vi7, 0(vi4)
    const int32_t ix0 = Wd[0] & 0x7FFF;
    Ps[0] = ldq(vu, static_cast<uint32_t>(636 + ix0));
    stqi(vu, static_cast<uint32_t>(vi6), _mm_set_epi32(static_cast<int>(m1012w), static_cast<int>(m1012z), static_cast<int>(vf27y), static_cast<int>(vf27x))); // SQI vf27
    vi6 = s16(vi6 + 1);
    Cs[0] = ldq(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi3)));
    int32_t pC = s16(vi3 + 1);
    As[0] = ldq(vu, static_cast<uint32_t>(716 + ix0));
    Ss[0] = ldq(vu, copyS ? static_cast<uint32_t>(796 + ix0) : static_cast<uint32_t>(static_cast<uint16_t>(pS)));
    if (!copyS)
        pS = s16(pS + 1);
    Fb[0] = clipBitsV(Ps[0]);
    vi1 = (Fb[0] & 0x3Fu) ? 1 : 0;
    {
        const __m128 p1 = fmacV<4>(c1v, Ps[0], zero);
        Tv[0] = fmacV<2>(c2v, bc<3>(Ps[0]), p1);
        const __m128 p3 = fmacV<4>(Cs[0], vf16, zero);
        const __m128 p5 = fmacV<4>(c14v, p3, zero);
        Lit[0] = fmacV<2>(As[0], p3, p5);
        Qf[0] = divQ(Tv[0]);
    }
    for (int32_t it = 1; it <= N; ++it)
    {
        const int32_t j = it - 1;
        vi10 = vi1;
        char pth;
        int hit = -1;
        if (Wd[j] < 0)
            pth = 'A';
        else if (vi8 <= 0)
            pth = 'B';
        else
        {
            pth = 'C';
            const uint32_t cl0 = j >= 2 ? Fb[j - 2] : 0u, cl1 = j >= 1 ? Fb[j - 1] : 0u, cl2 = Fb[j];
            const uint32_t vis = (cl0 << 12) | (cl1 << 6) | cl2;
            static constexpr uint32_t kMasks[6] = {0xFDF7DFu, 0xFEFBEFu, 0xFF7DF7u, 0xFFBEFBu, 0xFFDF7Du, 0xFFEFBEu};
            for (int m = 0; m < 6; ++m)
                if ((vis | kMasks[m]) == 0xFFFFFFu)
                {
                    hit = m;
                    break;
                }
            if (hit < 0)
            {
                if (!clipPolygon(vu, vi13, vi5, vi6, Wd[j], vi8, vi9, vi10, copyS, c1v, c2v, c5v,
                                 Ps[j - 2], Ps[j - 1], Ps[j], fmacV<0>(V7(Ss[j - 2]), zero, zero), V7(Ss[j - 1]), V7(Ss[j]),
                                 clampI(Lit[j - 2]), clampI(Lit[j - 1]), Lit[j], kicks, gif))
                    return fb1EE0(kFbRigidClip);
            }
            else if (hit == 5) // ISW.x vi5, 1022 (delay slot of the IBNE at 28F8)
                stqi(vu, 1022u, _mm_insert_epi32(ldqi(vu, 1022u), vi5, 0));
        }
        const __m128 qb = _mm_set1_ps(Qf[j]);
        const __m128 f2 = fmacV<4>(vf0, bc<2>(c5v), zero);
        const __m128 f3 = fmacV<2>(bc<3>(c5v), qb, f2);
        const __m128 f5 = fmacV<4>(Tv[j], qb, zero);
        const __m128 f6 = fmacV<4>(V7(Ss[j]), qb, zero);
        Wd[it] = s16(static_cast<int32_t>(ldw(vu, static_cast<uint32_t>(vi4_0 + it), 0) & 0xFFFFu));
        const int32_t ix = Wd[it] & 0x7FFF;
        Ps[it] = ldq(vu, static_cast<uint32_t>(636 + ix));
        vi6 = s16(vi6 + 4);
        {
            const uint32_t qd = static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 3));
            stq(vu, qd, _mm_blend_ps(ldq(vu, qd), f6, 0x7)); // lanes xyz only
        }
        As[it] = ldq(vu, static_cast<uint32_t>(716 + ix));
        Ss[it] = ldq(vu, copyS ? static_cast<uint32_t>(796 + ix) : static_cast<uint32_t>(static_cast<uint16_t>(pS)));
        if (!copyS)
            pS = s16(pS + 1);
        Cs[it] = ldq(vu, static_cast<uint32_t>(static_cast<uint16_t>(pC)));
        pC = s16(pC + 1);
        const __m128 f7 = fmacV<4>(c1v, Ps[it], zero);
        const __m128 tw = clampI(f3);
        const float vf15w = pth == 'B' ? 0.0f : 2048.0f;
        const __m128 f8 = _mm_add_ps(tw, _mm_set1_ps(vf15w)); // ADD.w (tw clamped to [0,255]: never out of the exponent window / never needs the slow path)
        Tv[it] = fmacV<2>(c2v, bc<3>(Ps[it]), f7);
        const __m128 f11 = fmacV<4>(Cs[it], vf16, zero);
        const __m128 f12 = fmacV<4>(c14v, f11, zero);
        Lit[it] = fmacV<2>(As[it], f11, f12);
        {
            const __m128i rg = ftoiS<1>(clampI(Lit[j]));
            __m128i xy = ftoiS<16>(f5);
            const __m128i ww = ftoiS<16>(f8);
            xy = _mm_insert_epi32(xy, _mm_extract_epi32(ww, 3), 3);
            stqi(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 2)), rg);
            stqi(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 4)), xy);
            stqi(vu, static_cast<uint32_t>(static_cast<uint16_t>(vi6 - 1)), xy);
        }
        Fb[it] = clipBitsV(Ps[it]);
        Qf[it] = divQ(Tv[it]);
        vi1 = (Fb[it] & 0x3Fu) ? 1 : 0;
        vi8 = s16(vi9 + vi10 + vi1);
        vi9 = vi10;
        vi5 = s16(vi5 - 1);
        vi4 = s16(vi4 + 1);
    }
    // epilogue 25B8..2200: ILW.w vi6 / ILW.z vi7 from 1023, XTOP vi12, ISW.z vi6 / ISW.w vi7 (swap), XGKICK vi7
    const uint32_t e6 = ilw(vu, 1023, 3), e7 = ilw(vu, 1023, 2);
    stqi(vu, 1023u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1023u), static_cast<int>(e6), 2), static_cast<int>(e7), 3));
    if (!kick(vu, e7, kicks, gif))
        return fb1EE0(kFbRigidKick);
    R.vi[12] = top; // XTOP vi12
    return true;
}

// PERF-SVF: 8-wide struct-of-arrays AVX2 vertex chain, copied from lbVertex8 / lb8Op / lb8FromRows / lb8ToRows (vu1_native_hand.inc lines ~601-812; flags/sticky/mac
// bookkeeping dropped, only the value + exactness guard kept). Exact whenever no result lane of any op leaves the exponent window [2,253] (except certain zeros);
// otherwise the caller runs the scalar fmacV chain for that group.
struct Fv8V
{
    __m256 c[4];
};
inline __m256 fv8N(__m256 v) // == lb8N == vuhot::N4 on 8 lanes
{
    const __m256i bits = _mm256_castps_si256(v);
    const __m256i expo = _mm256_and_si256(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(0xFF));
    const __m256i sign = _mm256_and_si256(bits, _mm256_set1_epi32(static_cast<int>(0x80000000u)));
    const __m256i isZero = _mm256_cmpeq_epi32(expo, _mm256_setzero_si256());
    const __m256i isFF = _mm256_cmpeq_epi32(expo, _mm256_set1_epi32(0xFF));
    __m256i r = _mm256_blendv_epi8(bits, sign, isZero);
    r = _mm256_blendv_epi8(r, _mm256_or_si256(sign, _mm256_set1_epi32(0x7F7FFFFF)), isFF);
    return _mm256_castsi256_ps(r);
}
inline __m256i fv8OutWin(__m256 v)
{
    const __m256i ev = _mm256_and_si256(_mm256_srli_epi32(_mm256_castps_si256(v), 23), _mm256_set1_epi32(0xFF));
    return _mm256_or_si256(_mm256_cmpgt_epi32(_mm256_set1_epi32(2), ev), _mm256_cmpgt_epi32(ev, _mm256_set1_epi32(253)));
}
inline __m256i fv8IsZ(__m256 v) { return _mm256_castps_si256(_mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_EQ_OQ)); }
inline bool fv8Any(__m256i m) { return !_mm256_testz_si256(m, m); }

template <int DEST, bool MADD>
inline Fv8V fv8Op(const Fv8V &a, const Fv8V &b, const Fv8V &ac, __m256i &bad)
{
    Fv8V r;
    for (int c = 0; c < 4; ++c)
    {
        if (((DEST >> (3 - c)) & 1) == 0)
        {
            r.c[c] = _mm256_setzero_ps();
            continue;
        }
        const __m256 av = a.c[c], bv = b.c[c];
        const __m256 val = MADD ? _mm256_fmadd_ps(av, bv, ac.c[c]) : _mm256_mul_ps(av, bv);
        r.c[c] = val;
        const __m256i za = _mm256_or_si256(fv8IsZ(av), fv8IsZ(bv));
        __m256i zok = _mm256_and_si256(fv8IsZ(val), za);
        if (MADD)
            zok = _mm256_and_si256(zok, fv8IsZ(ac.c[c]));
        bad = _mm256_or_si256(bad, _mm256_andnot_si256(zok, fv8OutWin(val)));
        if (MADD)
            bad = _mm256_or_si256(bad, _mm256_andnot_si256(za, fv8OutWin(_mm256_mul_ps(av, bv))));
    }
    return r;
}
inline Fv8V fv8Const(__m128 v)
{
    v = norm4(v);
    Fv8V r;
    r.c[0] = _mm256_broadcastss_ps(v);
    r.c[1] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0x55));
    r.c[2] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0xAA));
    r.c[3] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0xFF));
    return r;
}
inline Fv8V fv8FromRows(const __m128 rows[8])
{
    const __m256 r0 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[0]), rows[4], 1);
    const __m256 r1 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[1]), rows[5], 1);
    const __m256 r2 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[2]), rows[6], 1);
    const __m256 r3 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[3]), rows[7], 1);
    const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
    const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
    Fv8V r;
    r.c[0] = _mm256_shuffle_ps(t0, t1, 0x44);
    r.c[1] = _mm256_shuffle_ps(t0, t1, 0xEE);
    r.c[2] = _mm256_shuffle_ps(t2, t3, 0x44);
    r.c[3] = _mm256_shuffle_ps(t2, t3, 0xEE);
    return r;
}
inline void fv8ToRows(const Fv8V &v, __m128 rows[8])
{
    const __m256 t0 = _mm256_unpacklo_ps(v.c[0], v.c[1]), t1 = _mm256_unpacklo_ps(v.c[2], v.c[3]);
    const __m256 t2 = _mm256_unpackhi_ps(v.c[0], v.c[1]), t3 = _mm256_unpackhi_ps(v.c[2], v.c[3]);
    const __m256 r0 = _mm256_shuffle_ps(t0, t1, 0x44), r1 = _mm256_shuffle_ps(t0, t1, 0xEE);
    const __m256 r2 = _mm256_shuffle_ps(t2, t3, 0x44), r3 = _mm256_shuffle_ps(t2, t3, 0xEE);
    rows[0] = _mm256_castps256_ps128(r0);
    rows[4] = _mm256_extractf128_ps(r0, 1);
    rows[1] = _mm256_castps256_ps128(r1);
    rows[5] = _mm256_extractf128_ps(r1, 1);
    rows[2] = _mm256_castps256_ps128(r2);
    rows[6] = _mm256_extractf128_ps(r2, 1);
    rows[3] = _mm256_castps256_ps128(r3);
    rows[7] = _mm256_extractf128_ps(r3, 1);
}

// 1EE0 entry (vi10 = 0). Prologue 1EF8..1F78 in closed form, then the skinned path 1FE0 -> JR (1:1 value port of lbFull / lbVertex: the exact scalar vertex
// chain, no AVX2/relaxed groups), memory + GIF only. R.vi[12] is live state (the routine leaves vi12 = idx of vertex 1). Returns false (nothing needs undoing:
// the caller restores the undo log) for every case lbFull declines or that it does not model: N outside 4..67, DIV by zero, any aliasing between the output
// buffer and anything the real (software-pipelined) routine reads after its first stores, kick failure.
// 1EE0 (vi10 = 0) and 1EF0 (vi10 = 1: reads S from 796+idx in both skinned 2210 and rigid 2670).
static bool handler1EE0(Regs &R, uint8_t *vu, uint16_t &vi13, uint16_t top, bool vi10, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    const int32_t vi1s = static_cast<int16_t>(ilw(vu, 1011, 1)); // 1F00 ILW.y vi1, 1011 (< 0: rigid path at 2440 / 2670)
    const uint16_t vi12 = R.vi[12];
    const uint16_t w1018 = ilw(vu, 1018, 3); // vi5 (before the skinned path doubles it) = vertex count N
    const uint32_t s2base = static_cast<uint16_t>(ilw(vu, 1018, 0) + vi12); // vi2 = ILW.x 1018, += vi12 (2010 / 2490)
    const uint32_t s3base = static_cast<uint16_t>(ilw(vu, 1018, 1) + vi12); // vi3 = ILW.y 1018 + vi12 (1F60)
    const uint32_t i4base = static_cast<uint16_t>(ilw(vu, 1018, 2) + vi12); // vi4 = ILW.z 1018 + vi12 (1F48)
    const uint32_t ubase = ilw(vu, 1023, 2);                                // vi6 = ILW.z 1023 (output buffer)
    const uint16_t vi8 = static_cast<uint16_t>(w1018 + 32768u);              // two IADDIU 16384
    const uint32_t vf27x = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(vi8))); // MFIR.x vf27, vi8
    const uint32_t vf27y = ldw(vu, vi13, 1);                                                  // LQI.y vf27, (vi13++)
    if (vi1s < 0)
    {
        if (w1018 < 1u || w1018 > 300u || vi13 == 1022u || vi13 == 1023u || ((vi13 + 1u) & 0x3FFu) == 1023u)
            return fb1EE0(kFbRigidN);
        vi13 = static_cast<uint16_t>(vi13 + 1u);
        return rigid2440(R, vu, vi13, top, static_cast<int32_t>(w1018), i4base, s2base, s3base, ubase, vf27x, vf27y, vi10, kicks, gif);
    }
    // skinN guard: N in [1, 255]; every output qword ubase + 4N must fit in 1024 and each input stream (vi4/vi3/vi2 bases + N) must not wrap past 1024
    const auto skinNFail = [&]() {
        const uint32_t n = w1018;
        ++s_skinNHist[n <= 3u ? n : (n < 100u ? 4u : (n < 200u ? 5u : 6u))]; // 0..3, 68..99 (and 4..67 bounds fails), 100..199, 200+
        return fb1EE0(kFbSkinN);
    };
    if (w1018 < 1u || w1018 > kMaxSkinN)
        return skinNFail();
    const int32_t N = w1018;
    const uint32_t span = 4u * static_cast<uint32_t>(N) + 1u;
    if (ubase + span > 1024u || i4base + static_cast<uint32_t>(N) > 1024u || s3base + static_cast<uint32_t>(N) > 1024u || (!vi10 && s2base + static_cast<uint32_t>(N) > 1024u))
        return skinNFail();
    const auto hits = [&](uint32_t q) { return ((q - ubase) & 0x3FFu) < span; };
    if (hits(1012) || hits(1013) || hits(1023) || hits(vi13) || hits(vi13 + 1u))
        return fb1EE0(kFbSkinAlias13);
    vi13 = static_cast<uint16_t>(vi13 + 1u);

    const __m128 zero = _mm_setzero_ps();
    const __m128 vf1 = ldq(vu, 1016), vf2 = ldq(vu, 1014), vf3 = ldq(vu, 1002), vf4 = ldq(vu, 1003), vf16 = ldq(vu, 1013);
    const __m128 accw = fmacV<4>(_mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), bc<2>(vf1), zero); // MULAz.w ACC, vf0, vf1
    struct SkV
    {
        __m128 S2Q, prod, R7;
        float Wraw;
        int32_t idx;
    };
    SkV V[kMaxSkinN];
    __m128 rA[kMaxSkinN], rB[kMaxSkinN], rS3[kMaxSkinN], rS2[kMaxSkinN];
    for (int32_t t = 0; t < N; ++t) // gather + aliasing checks (same order of checks as before; any failure returns false before any store)
    {
        const int32_t idx = static_cast<int16_t>(ilw(vu, i4base + static_cast<uint32_t>(t), 0));
        const uint32_t qa = static_cast<uint32_t>(636 + idx), qb2 = static_cast<uint32_t>(716 + idx);
        const uint32_t q3 = static_cast<uint16_t>(s3base + static_cast<uint32_t>(t));
        const uint32_t q2 = vi10 ? static_cast<uint32_t>(796 + idx) : static_cast<uint16_t>(s2base + static_cast<uint32_t>(t));
        if (hits(i4base + static_cast<uint32_t>(t)) || hits(qa) || hits(qb2) || hits(q3) || hits(q2))
            return fb1EE0(kFbSkinGatherAlias);
        rA[t] = ldq(vu, qa);
        rB[t] = ldq(vu, qb2);
        rS3[t] = ldq(vu, q3);
        rS2[t] = ldq(vu, q2);
        V[t].idx = idx;
    }
    // PERF-SVF: groups of 8 vertices (short last group padded with copies of its last vertex), AVX2 SoA; guard failure -> scalar chain below for that group
    const Fv8V k1 = fv8Const(vf1), k2 = fv8Const(vf2), k3 = fv8Const(vf3), k4 = fv8Const(vf4), k16 = fv8Const(vf16), kaccw = fv8Const(accw);
    for (int32_t t0 = 0; t0 < N; t0 += 8)
    {
        const int32_t n = (N - t0 < 8) ? (N - t0) : 8;
        __m128 gA[8], gB[8], gS3[8], gS2[8];
        for (int j = 0; j < 8; ++j)
        {
            const int32_t s = t0 + (j < n ? j : n - 1);
            gA[j] = rA[s];
            gB[j] = rB[s];
            gS3[j] = rS3[s];
            gS2[j] = rS2[s];
        }
        Fv8V A = fv8FromRows(gA), B = fv8FromRows(gB), S3 = fv8FromRows(gS3), S2 = fv8FromRows(gS2);
        for (int c = 0; c < 4; ++c)
        {
            A.c[c] = fv8N(A.c[c]);
            B.c[c] = fv8N(B.c[c]);
            S3.c[c] = fv8N(S3.c[c]);
            S2.c[c] = fv8N(S2.c[c]);
        }
        Fv8V z8;
        z8.c[0] = z8.c[1] = z8.c[2] = z8.c[3] = _mm256_setzero_ps();
        __m256i bad = _mm256_setzero_si256();
        const Fv8V mula = fv8Op<15, false>(k3, A, z8, bad);
        const Fv8V t5 = fv8Op<15, true>(k4, Fv8V{{A.c[3], A.c[3], A.c[3], A.c[3]}}, mula, bad);
        const __m256 den = fv8N(t5.c[3]);
        bool ok = !fv8Any(fv8IsZ(den)); // zero den: scalar path below reports it (returns false)
        if (ok)
        {
            const __m256 qv = fv8N(fv8N(_mm256_div_ps(_mm256_set1_ps(1.0f), den)));
            const Fv8V Q{{qv, qv, qv, qv}};
            const Fv8V r7 = fv8Op<14, false>(t5, Q, z8, bad);
            const Fv8V os2q = fv8Op<14, false>(S2, Q, z8, bad);
            const Fv8V w = fv8Op<1, true>(k1, Q, kaccw, bad);
            const Fv8V bv = fv8Op<15, false>(B, k16, z8, bad);
            const Fv8V in = fv8Op<15, true>(k2, k16, bv, bad);
            const Fv8V pr = fv8Op<15, false>(S3, in, z8, bad);
            if (!fv8Any(bad))
            {
                __m128 oR7[8], oS2Q[8], oPr[8];
                fv8ToRows(r7, oR7);
                fv8ToRows(os2q, oS2Q);
                fv8ToRows(pr, oPr);
                alignas(32) float wf[8];
                _mm256_store_ps(wf, w.c[3]);
                for (int j = 0; j < n; ++j)
                {
                    V[t0 + j].R7 = oR7[j];
                    V[t0 + j].S2Q = oS2Q[j];
                    V[t0 + j].prod = oPr[j];
                    V[t0 + j].Wraw = wf[j];
                }
                continue;
            }
        }
        for (int32_t t = t0; t < t0 + n; ++t) // scalar fallback (the original chain)
        {
        const __m128 sA = rA[t], B = rB[t], S3 = rS3[t], S2 = rS2[t];
        const __m128 mula = fmacV<4>(vf3, sA, zero);
        const __m128 T5 = fmacV<2>(vf4, bc<3>(sA), mula);
        const float den = nrm1(_mm_cvtss_f32(bc<3>(T5)));
        if (den == 0.0f)
            return fb1EE0(kFbSkinDivZero); // lbFull declines (di != 0)
        const float Q = normRes(normRes(1.0f / den));
        const __m128 qb = _mm_set1_ps(Q);
        V[t].R7 = fmacV<4>(T5, qb, zero);
        V[t].S2Q = fmacV<4>(S2, qb, zero);
        V[t].Wraw = _mm_cvtss_f32(bc<3>(fmacV<2>(vf1, qb, accw)));
        const __m128 bv = fmacV<4>(B, vf16, zero);
        const __m128 in = fmacV<2>(vf2, vf16, bv);
        V[t].prod = fmacV<4>(S3, in, zero);
        }
    }
    const uint32_t m1012z = ldw(vu, 1012, 2), m1012w = ldw(vu, 1012, 3);
    const uint32_t ctlOld = ldw(vu, 1023, 3) & 0xFFFFu;
    // The whole output span [ubase, ubase + 4N] is logged once (the rest of the program may still fall back, so the log cannot be dropped), then written raw.
    logRange(vu, ubase, 4u * static_cast<uint32_t>(N) + 1u);
    stqRaw(vu, ubase, _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(m1012w), static_cast<int>(m1012z), static_cast<int>(vf27y), static_cast<int>(vf27x))));
    for (int32_t v = 0; v < N; ++v)
    {
        const uint32_t q0 = ubase + 1u + 4u * static_cast<uint32_t>(v);
        const int32_t w0 = _mm_cvtsi128_si32(ftoiS<16>(clampI(_mm_set1_ps(V[v].Wraw))));
        stqRaw(vu, q0, _mm_castsi128_ps(_mm_insert_epi32(ldqi(vu, q0), w0, 3)));
        stqRaw(vu, q0 + 1u, _mm_blend_ps(ldq(vu, q0 + 1u), V[v].S2Q, 0x7));
        stqRaw(vu, q0 + 2u, _mm_castsi128_ps(ftoiS<1>(clampI(V[v].prod))));
        stqRaw(vu, q0 + 3u, _mm_castsi128_ps(_mm_insert_epi32(ftoiS<16>(V[v].R7), static_cast<int32_t>(static_cast<uint32_t>(V[v].idx) & 0xFFFFu), 3)));
    }
    stqi(vu, 1023u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1023u), static_cast<int>(ctlOld), 2), static_cast<int>(ubase), 3)); // ISW.z / ISW.w toggle
    if (!kick(vu, ubase, kicks, gif)) // XGKICK vi6 (JR delay slot)
        return fb1EE0(kFbSkinKick);
    R.vi[12] = static_cast<uint16_t>(ilw(vu, (i4base + 1u) & 0x3FFu, 0)); // ILW.x vi12, 1(vi4) (read from VU mem: V[1] does not exist for N==1)
    return true;
}

// ---- Task F3 handlers: 0AF0, 0B40 (+0BC8/0C28), 15E0, 1900, 1C48 (+1C90/1CB0), 3160 (+3268) ----
static bool handler0AF0(Regs &R, uint8_t *vu, uint16_t vi13)
{
    const uint16_t vi3 = ilw(vu, 1011, 0), vi2 = ilw(vu, 1011, 3);
    const uint32_t count = static_cast<uint32_t>(vi2) + 1u;
    if (count > 512u)
        return false;
    const uint16_t base = static_cast<uint16_t>(vi3 + 716u);
    if (((vi13 - base) & 0x3FFu) < count)
        return false;
    const __m128 zero = _mm_setzero_ps();
    logRange(vu, base, count);
    for (uint32_t i = 0; i < count; ++i)
        stqRaw(vu, static_cast<uint32_t>(base) + i, zero);
    R.vf[1] = zero;
    R.vi[2] = vi2;
    R.vi[3] = vi3;
    R.vi[4] = static_cast<uint16_t>(base + vi2);
    return true;
}

static bool handler0B40(Regs &R, uint8_t *vu, uint16_t vi13)
{
    uint16_t vi1 = static_cast<uint16_t>(ilw(vu, 1018, 2) + R.vi[12]);
    const uint16_t n = ilw(vu, 1018, 3);
    if (n == 0u || n > 512u)
        return false;
    if (((vi13 - vi1) & 0x3FFu) < n)
        return false;
    const uint16_t vi2 = static_cast<uint16_t>(vi1 + n + 2u);
    const __m128 zero = _mm_setzero_ps();
    __m128 vf1 = _mm_blend_ps(_mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), ldq(vu, static_cast<uint32_t>(vi13) - 1u), 0x2);
    __m128 vf4 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    const __m128 vf11 = _mm_set1_ps(1.0f);
    uint16_t vi6 = ilw(vu, vi1, 0);
    const uint16_t vi5 = ilw(vu, static_cast<uint16_t>(vi1 + 1u), 0);
    __m128 vf12 = ldq(vu, static_cast<uint32_t>(636 + static_cast<int16_t>(vi6)));
    __m128 vf2 = ldq(vu, static_cast<uint32_t>(636 + static_cast<int16_t>(vi5)));
    vf12 = _mm_blend_ps(vf12, fmacV<0>(zero, bc<3>(vf12), zero), 0x4);
    vf2 = _mm_blend_ps(vf2, fmacV<0>(zero, bc<3>(vf2), zero), 0x4);
    __m128 vf3 = norm4(vf12);
    vi1 = static_cast<uint16_t>(vi1 + 2u);
    uint16_t vi7 = ilw(vu, vi1, 0);
    uint16_t vi8 = ilw(vu, static_cast<uint16_t>(vi1 - 2u), 1);
    uint16_t vi9 = 0u;
    __m128 vf5 = zero, acc = zero;
    bool macSignX = false; // 0BB0 MUL.x vf0, vf0, vf0 -> +0.0f (S=0)
    for (uint32_t k = 0; k < n; ++k)
    {
        vf5 = norm4(vf3);
        vi1 = static_cast<uint16_t>(vi1 + 1u);
        vf3 = norm4(vf2);
        vi9 = macSignX ? 128u : 0u; // 0BD0 FMAND vi9, vi3 (vi3=128) reads 0C10 MULy.x from k-1
        vf2 = ldq(vu, static_cast<uint32_t>(636 + static_cast<int16_t>(vi7)));
        acc = fmacV<0>(vf4, bc<1>(vf4), zero);
        vi9 = static_cast<uint16_t>(vi9 + 32767u);
        vf4 = _mm_blend_ps(vf4, fmacV<2>(vf11, bc<2>(vf4), acc), 0x1);
        vi8 = static_cast<uint16_t>(vi9 + vi8);
        const __m128 vf5_yzx = _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(3, 0, 2, 1));
        const __m128 vf3_zxy = _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 1, 0, 2));
        acc = fmacV<4>(vf5_yzx, vf3_zxy, zero);
        vi8 = static_cast<uint16_t>(vi8 & 0x8000u);
        const __m128 vf3_yzx = _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 0, 2, 1));
        const __m128 vf5_zxy = _mm_shuffle_ps(vf5, vf5, _MM_SHUFFLE(3, 1, 0, 2));
        vf5 = _mm_blend_ps(vf5, fmacV<3>(vf3_yzx, vf5_zxy, acc), 0x7);
        vi6 = static_cast<uint16_t>(vi6 | vi8);
        vf2 = _mm_blend_ps(vf2, fmacV<0>(zero, bc<3>(vf2), zero), 0x4);
        const uint32_t qdst = static_cast<uint16_t>(vi1 - 3u);
        stqi(vu, qdst, _mm_insert_epi32(ldqi(vu, qdst), static_cast<int32_t>(vi6), 0));
        vi6 = ilw(vu, static_cast<uint16_t>(vi1 - 2u), 0);
        // 0C10 MULy.x vf0, vf4, vf1: sign bit of exact double product (execUpper/fastUpper flagsOf)
        macSignX = std::signbit(static_cast<double>(nrm1(_mm_cvtss_f32(vf4))) * static_cast<double>(nrm1(_mm_cvtss_f32(bc<1>(vf1)))));
        vi7 = ilw(vu, vi1, 0);
        vf4 = fmacV<4>(vf5, vf2, zero);
        vi8 = ilw(vu, static_cast<uint16_t>(vi1 - 2u), 1);
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3; R.vf[4] = vf4; R.vf[5] = vf5; R.vf[11] = vf11; R.vf[12] = vf12;
    R.acc = acc;
    R.vi[2] = vi2; R.vi[3] = 128u; R.vi[4] = 0x8000u; R.vi[5] = vi5; R.vi[6] = vi6; R.vi[7] = vi7; R.vi[8] = vi8; R.vi[9] = vi9;
    return true;
}

static bool handler15E0(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t vi1 = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 512u)
        return false;
    const uint16_t base1 = static_cast<uint16_t>(vi1 + 556u), base2 = static_cast<uint16_t>(vi1 + 716u);
    const uint16_t nextVi13 = static_cast<uint16_t>(vi13 + 3u);
    if (((nextVi13 - base2) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), half = _mm_set1_ps(0.5f);
    __m128 vf3 = ldq(vu, vi13);
    const __m128 m0 = ldq(vu, 989), m1 = ldq(vu, 990), m2 = ldq(vu, 991);
    vf3 = fmacV<2>(m2, bc<2>(vf3), fmacV<2>(m1, bc<1>(vf3), fmacV<4>(m0, bc<0>(vf3), zero)));
    const __m128 vf1 = ldq(vu, static_cast<uint16_t>(vi13 + 1u));
    const __m128 vf2 = ldq(vu, static_cast<uint16_t>(vi13 + 2u));
    vi13 = nextVi13;
    const __m128 acc05 = fmacV<0>(zero, half, zero);
    logRange(vu, base2, n);
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 nrm = ldq(vu, static_cast<uint32_t>(base1) + k);
        const __m128 p = fmacV<4>(vf3, nrm, zero);
        const __m128 d = fmacV<0>(fmacV<0>(p, bc<1>(p), zero), bc<2>(p), zero);
        const __m128 pos = fmacV<2>(d, half, acc05);
        const __m128 neg = fmacV<3>(d, half, acc05);
        const __m128 c0 = ldq(vu, static_cast<uint32_t>(base2) + k);
        const __m128 ac0 = fmacV<0>(c0, zero, zero);
        const __m128 ac1 = fmacV<2>(vf1, bc<0>(pos), ac0);
        const __m128 out = fmacV<2>(vf2, bc<0>(neg), ac1);
        stqRaw(vu, static_cast<uint32_t>(base2) + k, out);
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3;
    return true;
}

static bool handler1900(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t vi2)
{
    const uint16_t vi11 = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n == 0u || n > 512u)
        return false;
    const uint16_t baseSrc = static_cast<uint16_t>(vi11 + 556u), baseDst = static_cast<uint16_t>(vi11 + 796u);
    if (((vi13 - baseDst) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 vf9 = ldq(vu, vi2), vf10 = ldq(vu, static_cast<uint32_t>(vi2) + 1u);
    const __m128 vf11 = ldq(vu, static_cast<uint32_t>(vi2) + 2u), vf12 = ldq(vu, static_cast<uint32_t>(vi2) + 3u);
    const __m128 m986 = ldq(vu, 986), m987 = ldq(vu, 987), m988 = ldq(vu, 988);
    const auto mulMat = [&](__m128 c) {
        __m128 a = fmacV<4>(vf9, bc<0>(c), zero);
        a = fmacV<2>(vf10, bc<1>(c), a);
        a = fmacV<2>(vf11, bc<2>(c), a);
        return fmacV<2>(vf12, bc<3>(c), a);
    };
    const __m128 r2 = mulMat(m987), r3 = mulMat(m988), r12 = mulMat(m986);
    logRange(vu, baseDst, n);
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 v = ldq(vu, static_cast<uint32_t>(baseSrc) + k);
        __m128 a = fmacV<4>(r12, bc<0>(v), zero);
        a = fmacV<2>(r2, bc<1>(v), a);
        a = fmacV<2>(r3, bc<2>(v), a);
        const __m128 res = fmacV<2>(vf12, one, a);
        const uint32_t qd = static_cast<uint32_t>(baseDst) + k;
        stqRaw(vu, qd, _mm_blend_ps(ldq(vu, qd), res, 0x7));
    }
    R.vf[2] = r2; R.vf[3] = r3; R.vf[4] = vf12; R.vf[12] = r12;
    return true;
}

static bool handler1C48(Regs &R, uint8_t *vu, uint16_t &vi13)
{
    const uint16_t vi1_0 = ilw(vu, 1018, 0), n = ilw(vu, 1018, 3);
    if (n == 0u || n > 1024u)
        return false;
    const __m128 vf1 = ldq(vu, vi13);
    const __m128 vf2 = ldq(vu, static_cast<uint16_t>(vi13 + 1u));
    const __m128 vf3 = ldq(vu, static_cast<uint16_t>(vi13 + 2u));
    const __m128 vf4 = ldq(vu, static_cast<uint16_t>(vi13 + 3u));
    vi13 = static_cast<uint16_t>(vi13 + 4u);
    const uint16_t base = static_cast<uint16_t>(vi1_0 + R.vi[12]);
    if (((vi13 - base) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    logRange(vu, base, n);
    __m128 vf6 = zero, acc = zero;
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 v = ldq(vu, static_cast<uint32_t>(base) + k);
        acc = fmacV<4>(vf1, bc<0>(v), zero);
        acc = fmacV<2>(vf2, bc<1>(v), acc);
        acc = fmacV<2>(vf3, bc<2>(v), acc);
        vf6 = fmacV<2>(vf4, one, acc);
        stqRaw(vu, static_cast<uint32_t>(base) + k, vf6);
    }
    R.vf[1] = vf1; R.vf[2] = vf2; R.vf[3] = vf3; R.vf[4] = vf4; R.vf[6] = vf6;
    R.acc = acc;
    R.vi[2] = static_cast<uint16_t>(base + n + 1u);
    return true;
}

static bool handler3160(Regs &R, uint8_t *vu, uint16_t vi13)
{
    const uint16_t vi1 = ilw(vu, 1011, 0), n = ilw(vu, 1011, 3);
    if (n < 2u || n > 512u)
        return false;
    const uint16_t base = static_cast<uint16_t>(vi1 + 636u);
    if (((vi13 - base) & 0x3FFu) < n)
        return false;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    __m128 vf4 = ldq(vu, base);
    __m128 vf1 = fmacV<1>(ldq(vu, base), one, zero); // SUBi.w vf1, vf1, vf0 (I = 1.0f)
    for (uint32_t k = 1; k < n; ++k)
    {
        const __m128 vf2 = ldq(vu, static_cast<uint32_t>(base) + k);
        vf4 = miniV(vf4, vf1);
        vf1 = fmacV<1>(vf2, one, zero);
    }
    vf4 = miniV(vf4, vf1);
    const __m128 vf3c = ldq(vu, 1008), vf1z = ldq(vu, 1006), vf2z = ldq(vu, 1007);
    __m128 v1x = fmacV<1>(zero, bc<3>(vf4), zero); // 31E8 SUBw.x vf1, vf0, vf4
    v1x = maxV(v1x, vf3c);                         // 31F0 MAX.x vf1, vf1, vf3
    const __m128 v2x = miniV(v1x, zero);           // 31F8 MINI.x vf2, vf1, vf0
    const __m128 inp0 = ldq(vu, base);
    const __m128 v2w = fmacV<0>(bc<3>(inp0), bc<0>(v2x), zero); // 3200 ADDx.w vf2, vf1, vf2
    const float q = divQ2(_mm_cvtss_f32(v2w), _mm_cvtss_f32(bc<3>(inp0)));
    const __m128 qb = _mm_set1_ps(q);
    logRange(vu, base, n);
    __m128 vf3 = zero, acc = zero;
    for (uint32_t k = 0; k < n; ++k)
    {
        const __m128 inp = ldq(vu, static_cast<uint32_t>(base) + k);
        const __m128 xyw = fmacV<4>(inp, qb, zero);
        acc = fmacV<4>(vf1z, bc<3>(xyw), zero);
        const __m128 z = fmacV<2>(vf2z, one, acc);
        vf3 = _mm_blend_ps(xyw, z, 0x4);
        stqRaw(vu, static_cast<uint32_t>(base) + k, vf3);
    }
    R.vf[3] = vf3; R.vf[4] = vf4;
    R.acc = acc;
    return true;
}

static bool vu1FuncRunInner(uint8_t *vu, uint32_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif, bool oneShot)
{
    Regs R;
    std::memset(&R, 0, sizeof(R));
    R.vi[12] = static_cast<uint16_t>(top);
    uint16_t vi13 = R.vi[12];
    uint16_t vi1 = ilw(vu, vi13, 0);
    for (uint32_t step = 0; step < 100000u; ++step)
    {
        vi13 = static_cast<uint16_t>(vi13 + 1u);                      // 0028 IADDIU vi13, vi13, 1
        const uint16_t vi2 = ilw(vu, static_cast<uint32_t>(vi13) - 1u, 1); // 0038 ILW.y vi2, -1(vi13)
        const uint32_t h = (static_cast<uint32_t>(vi1) << 3) & 0x3FFFu;
        switch (h)
        {
        case 0x0000u: // program end (E bit)
            return true;
        case 0x0050u: // XGKICK 1023
            if (!kick(vu, 1023u, kicks, gif))
                return false;
            break;
        case 0x0060u: // JR vi15 only
            break;
        case 0x0070u: // vi2 -= vi12; vi13 = vi2 + vi12 (stream jump)
            vi13 = vi2;
            break;
        case 0x0088u: // XGKICK vi13; vi13 += vi2
            if (!kick(vu, vi13, kicks, gif))
                return false;
            vi13 = static_cast<uint16_t>(vi13 + vi2);
            break;
        case 0x0C80u: // copy loop: vi3 = z of the header; do { vf1 = LQI(vi13++); SQ vf1 -> (vi3++) } while (vi3 != vi2)
        {
            uint16_t vi3 = ilw(vu, static_cast<uint32_t>(vi13) - 1u, 2);
            const uint32_t count = static_cast<uint16_t>(vi2 - vi3);
            if (count == 0u || count > 1024u)
            {
                s_fbPc = 0xF020u;
                return false;
            }
            __m128 vf1 = _mm_setzero_ps();
            do
            {
                vf1 = ldq(vu, vi13);
                vi13 = static_cast<uint16_t>(vi13 + 1u);
                vi3 = static_cast<uint16_t>(vi3 + 1u);
                stq(vu, static_cast<uint32_t>(vi3) - 1u, vf1);
            } while (vi3 != vi2);
            R.vf[1] = vf1;
            R.vi[3] = vi3;
            break;
        }
        case 0x0AF0u:
            if (!handler0AF0(R, vu, vi13))
            {
                s_fbPc = 0x0AF0u;
                return false;
            }
            break;
        case 0x0B40u:
            if (!handler0B40(R, vu, vi13))
            {
                s_fbPc = 0x0B40u;
                return false;
            }
            break;
        case 0x15E0u:
            if (!handler15E0(R, vu, vi13))
            {
                s_fbPc = 0x15E0u;
                return false;
            }
            break;
        case 0x1900u:
            if (!handler1900(R, vu, vi13, vi2))
            {
                s_fbPc = 0x1900u;
                return false;
            }
            break;
        case 0x1C48u:
            if (!handler1C48(R, vu, vi13))
            {
                s_fbPc = 0x1C48u;
                return false;
            }
            break;
        case 0x1EE0u: // Task F2: mesh transform (vi10 = 0 entry)
            if (!handler1EE0(R, vu, vi13, static_cast<uint16_t>(top), false, kicks, gif))
            {
                s_fbPc = 0x1EE0u;
                return false;
            }
            break;
        case 0x1EF0u: // Task F3: mesh transform (vi10 = 1 entry, reads S from 796+idx)
            if (!handler1EE0(R, vu, vi13, static_cast<uint16_t>(top), true, kicks, gif))
            {
                s_fbPc = 0x1EF0u;
                return false;
            }
            break;
        case 0x3160u:
            if (!handler3160(R, vu, vi13))
            {
                s_fbPc = 0x3160u;
                return false;
            }
            break;
        case 0x00A8u:
            if (!handler00A8(R, vu))
            {
                s_fbPc = 0x00A8u;
                return false;
            }
            break;
        default:
            s_fbPc = h;
            return false;
        }
        if (oneShot) // startPC 0040: vi15 = 0, so the handler's JR vi15 lands on 0000 (E bit) -> program ends after one handler
            return true;
        vi1 = ilw(vu, vi13, 0); // every handler ends with ILW.x vi1, 0(vi13)
    }
    s_fbPc = 0xF030u; // step budget
    return false;
}

// startPC / TOP / ITOP: only the main entry 0008 is supported (TOP is read by XTOP at 0010; ITOP is not used by the supported handlers).
static bool vu1FuncRun(uint8_t *vu, uint32_t startPC, uint32_t top, uint32_t itop, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    (void)itop;
    kicks.clear();
    gif.clear();
    s_fbPc = 0u;
    const bool oneShot = (startPC & 0xFFFFu) == 0x40u; // 0040: vi15 := 0, then entry at 0010; one handler, then END
    if ((startPC & 0xFFFFu) != 8u && !oneShot)
    {
        s_fbPc = 0xF001u;
        ++s_startPcs[startPC & 0xFFFFu];
        return false;
    }
    // Learned skip: 256-entry direct-mapped memo keyed by the first vi1 (ILW.x at TOP); 4+ consecutive fallbacks -> skip, every 64th still tries.
    struct Memo
    {
        uint16_t key;
        uint32_t streak;
    };
    static thread_local Memo memo[256] = {};
    static thread_local uint32_t skipCount = 0;
    static const bool memoOn = []() { const char *e = std::getenv("PS2X_VU1_FUNC_MEMO"); return e && e[0] && e[0] != '0'; }();
    const uint16_t first = memoOn ? ilw(vu, top, 0) : static_cast<uint16_t>(0);
    Memo dummy = {};
    Memo &m = memoOn ? memo[first & 0xFFu] : dummy;
    if (memoOn)
    {
        if (m.key != first)
        {
            m.key = first;
            m.streak = 0;
        }
        if (m.streak >= 4u && (++skipCount & 63u) != 0u)
        {
            s_fbPc = 0xF040u;
            return false;
        }
    }
    std::vector<UndoEnt> &log = undoLog();
    log.clear();
    if (vu1FuncRunInner(vu, top, kicks, gif, oneShot))
    {
        if (memoOn)
            m.streak = 0;
        return true;
    }
    if (memoOn)
        ++m.streak;
    for (size_t i = log.size(); i-- > 0;)
        std::memcpy(vu + (static_cast<uint32_t>(log[i].qword) << 4), log[i].old, 16);
    log.clear();
    kicks.clear();
    gif.clear();
    return false;
}

inline bool funcOn()
{
    // REVERSAL: PS2X_VU1_FUNC=0 restores the exact path (docs/REVERSAL_MEMO.md)
    static const bool on = []() { const char *e = std::getenv("PS2X_VU1_FUNC"); return !(e && e[0] == '0'); }();
    return on;
}
inline bool replayFuncOn()
{
    static const bool on = []() { const char *e = std::getenv("PS2X_VU1_REPLAY_FUNC"); return e && e[0] == '1'; }();
    return on;
}

static void reportMaybe()
{
    static uint32_t calls = 0;
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    if ((++calls & 0x3Fu) != 0u)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 5000)
        return;
    last = now;
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const auto &kv : s_fbPcs)
        v.emplace_back(kv.second, kv.first);
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    char buf[768];
    int n = std::snprintf(buf, sizeof(buf), "[vfunc] taken=%llu fallback=%llu fb_pcs:", (unsigned long long)s_taken, (unsigned long long)s_fallback);
    for (size_t i = 0; i < v.size() && i < 8u && n > 0 && n < 440; ++i)
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %04X:%llu", v[i].second, (unsigned long long)v[i].first);
    {
        std::vector<std::pair<uint64_t, uint32_t>> sp;
        for (const auto &kv : s_startPcs)
            sp.emplace_back(kv.second, kv.first);
        std::sort(sp.begin(), sp.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        if (n > 0 && n < 440)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " start_pcs:");
        for (size_t i = 0; i < sp.size() && i < 6u && n > 0 && n < 490; ++i)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %04X:%llu", sp[i].second, (unsigned long long)sp[i].first);
    }
    {
        if (n > 0 && n < 490)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " 1ee0_why:");
        for (int r = 0; r < kFbCount && n > 0 && n < 500; ++r)
            if (s_fb1EE0[r] != 0u)
                n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s:%llu", kFb1EE0Names[r], (unsigned long long)s_fb1EE0[r]);
    }
    if (s_fb1EE0[kFbSkinN] != 0u && n > 0 && n < 700)
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " skinN: 0:%llu 1:%llu 2:%llu 3:%llu 4-99:%llu 100-199:%llu 200+:%llu",
                           (unsigned long long)s_skinNHist[0], (unsigned long long)s_skinNHist[1], (unsigned long long)s_skinNHist[2], (unsigned long long)s_skinNHist[3],
                           (unsigned long long)s_skinNHist[4], (unsigned long long)s_skinNHist[5], (unsigned long long)s_skinNHist[6]);
    ps2xSafeLog("%s\n", buf);
}

// Engine + GIF submission (same order / destinations as finishXgkick). Returns true when the program was executed functionally.
static bool vu1FuncExec(uint8_t *vuData, uint32_t dataSize, uint32_t startPC, uint32_t top, uint32_t itop, GS &gs, PS2Memory *memory, std::vector<uint8_t> *capture, bool stats)
{
    static thread_local std::vector<std::pair<uint32_t, uint32_t>> kicks;
    static thread_local std::vector<uint8_t> gif;
    bool ok = false;
    if (dataSize == 16384u)
        ok = vu1FuncRun(vuData, startPC, top, itop, kicks, gif);
    else
        s_fbPc = 0xF002u;
    if (stats)
    {
        if (ok)
            ++s_taken;
        else
        {
            ++s_fallback;
            ++s_fbPcs[s_fbPc];
        }
        if (!capture)
            reportMaybe();
    }
    if (!ok)
        return false;
    {
        // PERF: kicks are normally appended back to back in `gif`; then submit the whole stream once (one arbiter pass instead of one per kick).
        uint32_t expect = 0u;
        bool contiguous = true;
        for (const auto &k : kicks)
        {
            if (k.second == 0u)
                continue;
            if (k.first != expect)
            {
                contiguous = false;
                break;
            }
            expect += k.second;
        }
        if (contiguous && expect == gif.size())
        {
            if (expect != 0u)
            {
                if (capture)
                    capture->insert(capture->end(), gif.data(), gif.data() + expect);
                else if (memory)
                    memory->submitGifPacket(GifPathId::Path1, gif.data(), expect);
                else
                    gs.processGIFPacket(gif.data(), expect);
            }
            return true;
        }
    }
    for (const auto &k : kicks)
    {
        if (k.second == 0u)
            continue;
        const uint8_t *p = gif.data() + k.first;
        if (capture)
            capture->insert(capture->end(), p, p + k.second);
        else if (memory)
            memory->submitGifPacket(GifPathId::Path1, p, k.second);
        else
            gs.processGIFPacket(p, k.second);
    }
    return true;
}
} // namespace vu1func
