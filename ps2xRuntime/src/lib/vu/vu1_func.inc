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
