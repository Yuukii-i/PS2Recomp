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

// KIND 4 = MUL (a*b, ac ignored), KIND 2 = MADD (ac + a*b). All four lanes (DEST = xyzw). Returns the 4 result lanes.
template <int KIND>
inline __m128 fmacV(__m128 a, __m128 b, __m128 ac)
{
    const __m128 a4 = norm4(a), b4 = norm4(b);
    __m128 ac4 = _mm_setzero_ps(), val, prodf = _mm_setzero_ps();
    uint32_t bad;
    if constexpr (KIND == 4)
    {
        val = _mm_mul_ps(a4, b4);
        bad = outOfWindow(val);
    }
    else
    {
        ac4 = norm4(ac);
        val = _mm_fmadd_ps(a4, b4, ac4);
        prodf = _mm_mul_ps(a4, b4);
        bad = outOfWindow(val) | outOfWindow(prodf);
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
        const double ex = (KIND == 4) ? ad * bd : static_cast<double>(C[l]) + ad * bd;
        const double mag = std::fabs(ex);
        const bool zero = mag == 0.0;
        const bool over = mag > static_cast<double>(FLT_MAX);
        const bool under = (mag < static_cast<double>(FLT_MIN)) && !zero;
        const uint32_t sg = std::signbit(ex) ? 0x80000000u : 0u;
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
static std::unordered_map<uint32_t, uint64_t> s_fbPcs; // handler pc (or 0xF000 + reason) -> count
static uint32_t s_fbPc = 0;

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

// Rigid path 2440 (per-vertex transform / trivial-reject / assemble, loop 24C8..25B0): 1:1 value + memory port of lbVert (vu1_native_hand.inc), which is exact on every
// record without real clipping. Differences: runs in place on VU memory through stq (same program order of loads/stores as lbVert's private copy, so the data
// seen by later loads is identical); flags / STATUS / cycles / exit registers dropped; the epilogue 25B8..2200 (1023 swap, XGKICK vi7, XTOP) added.
// Real clipping (no mask of the 6 trivial-reject tests matches, routine 2900+) -> false (whole program falls back).
static bool rigid2440(Regs &R, uint8_t *vu, uint16_t vi13, uint16_t top, int32_t N, uint32_t i4base, uint32_t s2base, uint32_t s3base, uint32_t ubase, uint32_t vf27x, uint32_t vf27y,
                      std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    (void)vi13;
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
    Ss[0] = ldq(vu, static_cast<uint32_t>(static_cast<uint16_t>(pS)));
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
                return false; // real clipping (2900+)
            if (hit == 5) // ISW.x vi5, 1022 (delay slot of the IBNE at 28F8)
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
        Ss[it] = ldq(vu, static_cast<uint32_t>(static_cast<uint16_t>(pS)));
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
        return false;
    R.vi[12] = top; // XTOP vi12
    return true;
}

// 1EE0 entry (vi10 = 0). Prologue 1EF8..1F78 in closed form, then the skinned path 1FE0 -> JR (1:1 value port of lbFull / lbVertex: the exact scalar vertex
// chain, no AVX2/relaxed groups), memory + GIF only. R.vi[12] is live state (the routine leaves vi12 = idx of vertex 1). Returns false (nothing needs undoing:
// the caller restores the undo log) for every case lbFull declines or that it does not model: N outside 4..67, DIV by zero, any aliasing between the output
// buffer and anything the real (software-pipelined) routine reads after its first stores, kick failure.
static bool handler1EE0(Regs &R, uint8_t *vu, uint16_t &vi13, uint16_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
{
    const int32_t vi1s = static_cast<int16_t>(ilw(vu, 1011, 1)); // 1F00 ILW.y vi1, 1011 (< 0: rigid path at 2440)
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
            return false;
        vi13 = static_cast<uint16_t>(vi13 + 1u);
        return rigid2440(R, vu, vi13, top, static_cast<int32_t>(w1018), i4base, s2base, s3base, ubase, vf27x, vf27y, kicks, gif);
    }
    if (w1018 < 4u || w1018 > 67u)
        return false;
    const int32_t N = w1018;
    const uint32_t span = 4u * static_cast<uint32_t>(N) + 1u;
    const auto hits = [&](uint32_t q) { return ((q - ubase) & 0x3FFu) < span; };
    if (hits(1012) || hits(1013) || hits(1023) || hits(vi13) || hits(vi13 + 1u))
        return false;
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
    SkV V[67];
    for (int32_t t = 0; t < N; ++t)
    {
        const int32_t idx = static_cast<int16_t>(ilw(vu, i4base + static_cast<uint32_t>(t), 0));
        const uint32_t qa = static_cast<uint32_t>(636 + idx), qb2 = static_cast<uint32_t>(716 + idx);
        const uint32_t q3 = static_cast<uint16_t>(s3base + static_cast<uint32_t>(t)), q2 = static_cast<uint16_t>(s2base + static_cast<uint32_t>(t));
        if (hits(i4base + static_cast<uint32_t>(t)) || hits(qa) || hits(qb2) || hits(q3) || hits(q2))
            return false;
        const __m128 A = ldq(vu, qa), B = ldq(vu, qb2), S3 = ldq(vu, q3), S2 = ldq(vu, q2);
        const __m128 mula = fmacV<4>(vf3, A, zero);
        const __m128 T5 = fmacV<2>(vf4, bc<3>(A), mula);
        const float den = nrm1(_mm_cvtss_f32(bc<3>(T5)));
        if (den == 0.0f)
            return false; // lbFull declines (di != 0)
        const float Q = normRes(normRes(1.0f / den));
        const __m128 qb = _mm_set1_ps(Q);
        V[t].R7 = fmacV<4>(T5, qb, zero);
        V[t].S2Q = fmacV<4>(S2, qb, zero);
        V[t].Wraw = _mm_cvtss_f32(bc<3>(fmacV<2>(vf1, qb, accw)));
        const __m128 bv = fmacV<4>(B, vf16, zero);
        const __m128 in = fmacV<2>(vf2, vf16, bv);
        V[t].prod = fmacV<4>(S3, in, zero);
        V[t].idx = idx;
    }
    const uint32_t m1012z = ldw(vu, 1012, 2), m1012w = ldw(vu, 1012, 3);
    const uint32_t ctlOld = ldw(vu, 1023, 3) & 0xFFFFu;
    stqi(vu, ubase, _mm_set_epi32(static_cast<int>(m1012w), static_cast<int>(m1012z), static_cast<int>(vf27y), static_cast<int>(vf27x)));
    for (int32_t v = 0; v < N; ++v)
    {
        const uint32_t q0 = ubase + 1u + 4u * static_cast<uint32_t>(v);
        const int32_t w0 = _mm_cvtsi128_si32(ftoiS<16>(clampI(_mm_set1_ps(V[v].Wraw))));
        stqi(vu, q0, _mm_insert_epi32(ldqi(vu, q0), w0, 3));
        stq(vu, q0 + 1u, _mm_blend_ps(ldq(vu, q0 + 1u), V[v].S2Q, 0x7));
        stqi(vu, q0 + 2u, ftoiS<1>(clampI(V[v].prod)));
        stqi(vu, q0 + 3u, _mm_insert_epi32(ftoiS<16>(V[v].R7), static_cast<int32_t>(static_cast<uint32_t>(V[v].idx) & 0xFFFFu), 3));
    }
    stqi(vu, 1023u, _mm_insert_epi32(_mm_insert_epi32(ldqi(vu, 1023u), static_cast<int>(ctlOld), 2), static_cast<int>(ubase), 3)); // ISW.z / ISW.w toggle
    if (!kick(vu, ubase, kicks, gif)) // XGKICK vi6 (JR delay slot)
        return false;
    R.vi[12] = static_cast<uint16_t>(V[1].idx); // ILW.x vi12, 1(vi4)
    return true;
}

static bool vu1FuncRunInner(uint8_t *vu, uint32_t top, std::vector<std::pair<uint32_t, uint32_t>> &kicks, std::vector<uint8_t> &gif)
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
        case 0x1EE0u: // Task F2: mesh transform (vi10 = 0 entry; 1EF0 = vi10 = 1 entry falls back via default)
            if (!handler1EE0(R, vu, vi13, static_cast<uint16_t>(top), kicks, gif))
            {
                s_fbPc = 0x1EE0u;
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
    if ((startPC & 0xFFFFu) != 8u)
    {
        s_fbPc = 0xF001u;
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
    if (vu1FuncRunInner(vu, top, kicks, gif))
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
    static const bool on = []() { const char *e = std::getenv("PS2X_VU1_FUNC"); return e && e[0] == '1'; }();
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
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf), "[vfunc] taken=%llu fallback=%llu fb_pcs:", (unsigned long long)s_taken, (unsigned long long)s_fallback);
    for (size_t i = 0; i < v.size() && i < 8u && n > 0 && n < 440; ++i)
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %04X:%llu", v[i].second, (unsigned long long)v[i].first);
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
