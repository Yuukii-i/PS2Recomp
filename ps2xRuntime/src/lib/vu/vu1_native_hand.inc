// HAND-MAINTAINED. Not produced by tools/vu1gen/gen_vu1_native.py (a dry run on 2026-11-04 shows the generator reproduces the vu1_native_exec_*.cpp
// files byte for byte, but not this code).
// This is the part of vu1_native_dispatch.cpp that was added by hand and by the patch_*.py scripts: the survey/capture/replay probes, the per-pc
// timers, the Level B kernels (lbFull, lbCopy, lbVert, lbLoop, lb8 SIMD), the XGKICK-overlap plumbing and the microcode image variants.
// It is #included into vu1_native_dispatch.cpp at the point where it used to sit, so the compiled result is the same as before the move.

struct SvPcGuard
{
    const uint64_t &cyc;
    uint32_t prev = 0xFFFFFFFFu;
    uint64_t c0 = 0;
    explicit SvPcGuard(const uint64_t &c) : cyc(c) {}
    void at(uint32_t pc)
    {
        if (prev != 0xFFFFFFFFu)
        {
            s_svC[prev >> 3] += cyc - c0;
            const uint32_t e = (prev >> 3) * 2048u + (pc >> 3);
            uint64_t &w = s_svEdge[e >> 6];
            const uint64_t m = 1ull << (e & 63u);
            if (!(w & m))
            {
                w |= m;
                ++s_svCov;
            }
        }
        prev = pc;
        c0 = cyc;
        if (s_svN[pc >> 3]++ == 0u) ++s_svCov;
        if ((++s_svIter & 0x1FFFFFull) == 0u) svPcDump();
    }
    ~SvPcGuard() { if (prev != 0xFFFFFFFFu) s_svC[prev >> 3] += cyc - c0; }
};

// PROBE-REGION (PS2X_VU1_REGION=1 -> logs/vu1region.bin): entry/exit snapshots of the skinned-vertex loop region [1FE0,2200) for the Level B kernel.
// Record: u32 'RGN ', u32 bytes, then ENTRY {u32 pc, u64 cycle, state, u32 dsz, data, pending} EXIT {u32 pc, u64 cycle, state, u32 nDiff, {u32 qw, 16B}*, pending}.
// pending = vf{i64 rel,u8 reg,u8 mask,f32[4]}* vi{i64 rel,u8 reg,i32}* acc{i64 rel,u8 mask,f32[4]}* flag{i64 rel,i64 issueRel,u32 mac,status,sticky,clip,u8 bits}*
//           store{i64 rel,u32 addr,u32[4],u8 mask}* then fdiv,efu0,efu1 {u8 valid,i64 rel,f32,u32 statusDi} and u8 xgkick.active.
struct RgRange
{
    uint32_t a, b, seen, kept, stride;
    std::map<uint32_t, std::array<uint32_t, 2>> byPc; // entry pc -> {entries seen, records kept}: every handler gets its own sampling
};
std::vector<RgRange> s_rgRanges = []()
{
    std::vector<RgRange> v;
    const char *e = std::getenv("PS2X_VU1_REGIONS");
    if (e)
    {
        while (*e)
        {
            char *end = nullptr;
            const unsigned long a = std::strtoul(e, &end, 16);
            if (!end || *end != ':')
                break;
            const unsigned long b = std::strtoul(end + 1, &end, 16);
            v.push_back(RgRange{static_cast<uint32_t>(a), static_cast<uint32_t>(b), 0u, 0u, 16u, {}});
            if (*end != ',')
                break;
            e = end + 1;
        }
    }
    else
    {
        const char *r = std::getenv("PS2X_VU1_REGION");
        if (r && r[0] == '1')
            v.push_back(RgRange{0x1FE0u, 0x2200u, 0u, 0u, 16u, {}});
    }
    return v;
}();
const bool s_rgOn = !s_rgRanges.empty();
const uint32_t s_rgCap = []() { const char *e = std::getenv("PS2X_VU1_REGION_CAP"); const unsigned long n = e ? std::strtoul(e, nullptr, 10) : 300ul; return static_cast<uint32_t>(n ? n : 300ul); }();
struct RgBuf
{
    std::vector<uint8_t> b;
    template <class T> void put(const T &v) { const uint8_t *p = reinterpret_cast<const uint8_t *>(&v); b.insert(b.end(), p, p + sizeof(T)); }
    void raw(const void *p, size_t n) { const uint8_t *q = static_cast<const uint8_t *>(p); b.insert(b.end(), q, q + n); }
};
void rgPending(RgBuf &o, VU1Interpreter &vu, uint64_t cyc)
{
    const auto rel = [&](uint64_t t) { return static_cast<int64_t>(t) - static_cast<int64_t>(cyc); };
    uint32_t n = 0;
    for (const auto &w : vu.m_vfWritePipeline) n += w.valid;
    o.put(n);
    for (const auto &w : vu.m_vfWritePipeline) if (w.valid) { o.put(rel(w.readyCycle)); o.put(w.reg); o.put(w.laneMask); o.raw(w.value.data(), 16); }
    n = 0;
    for (const auto &w : vu.m_viWritePipeline) n += w.valid;
    o.put(n);
    for (const auto &w : vu.m_viWritePipeline) if (w.valid) { o.put(rel(w.readyCycle)); o.put(w.reg); o.put(w.value); }
    n = 0;
    for (const auto &w : vu.m_accWritePipeline) n += w.valid;
    o.put(n);
    for (const auto &w : vu.m_accWritePipeline) if (w.valid) { o.put(rel(w.readyCycle)); o.put(w.laneMask); o.raw(w.value.data(), 16); }
    n = 0;
    for (const auto &w : vu.m_flagPipeline) n += w.valid;
    o.put(n);
    for (const auto &w : vu.m_flagPipeline)
        if (w.valid)
        {
            o.put(rel(w.readyCycle)); o.put(rel(w.issueCycle)); o.put(w.mac); o.put(w.status); o.put(w.extraSticky); o.put(w.clip);
            const uint8_t bits = uint8_t((w.writesMac ? 1 : 0) | (w.writesStatus ? 2 : 0) | (w.writesSticky ? 4 : 0) | (w.writesClip ? 8 : 0));
            o.put(bits);
        }
    n = 0;
    for (const auto &w : vu.m_storePipeline) n += w.valid;
    o.put(n);
    for (const auto &w : vu.m_storePipeline) if (w.valid) { o.put(rel(w.readyCycle)); o.put(w.address); o.raw(w.words.data(), 16); o.put(w.laneMask); }
    const auto sc = [&](const auto &e) { const uint8_t v = e.valid ? 1 : 0; o.put(v); o.put(rel(e.readyCycle)); o.put(e.value); o.put(e.statusDi); };
    sc(vu.m_fdiv); sc(vu.m_efu[0]); sc(vu.m_efu[1]);
    const uint8_t xg = vu.m_xgkick.active ? 1 : 0;
    o.put(xg);
}
int s_rgActive = -1;
uint32_t s_rgEntryPc = 0;
RgBuf s_rgRec;
RgBuf s_rgRing;
std::vector<uint8_t> s_rgMem0;
std::vector<uint16_t> s_rgTrace; // dispatch pcs visited inside the active region run (capped)
FILE *s_rgFile = nullptr;
// Record 'RGM ': u32 region index, u64 entry block hash, then ENTRY and EXIT as documented above, u64 exit block hash, u32 trace length, u16 trace pcs.
// A region run starts at the first dispatch pc inside [a,b) (not while an XGKICK runs or a branch is pending) and ends at the first dispatch pc outside it.
// Sampling is per (region, entry pc): the first 100 entries, then sparser and sparser as more are kept (cap PS2X_VU1_REGION_CAP per entry pc).
void rgStep(VU1Interpreter &vu, VuNativeCtx &c, uint32_t pc)
{
    if (s_rgActive < 0)
    {
        for (size_t i = 0; i < s_rgRanges.size(); ++i)
        {
            RgRange &r = s_rgRanges[i];
            if (pc < r.a || pc >= r.b || vu.m_xgkick.active || vu.m_state.branchPending || vu.m_state.ebit)
                continue;
            std::array<uint32_t, 2> &kv = r.byPc[pc];
            if (kv[1] >= s_rgCap)
                return;
            const uint32_t seen = kv[0]++;
            const uint32_t shift = kv[1] > 100u ? (std::min)((kv[1] - 100u) / 50u, 16u) : 0u;
            const uint32_t stride = 16u << shift; // the more a handler has kept, the sparser it samples, so the records span the whole session
            if (seen >= 100u && ((seen - 100u) % stride) != 0u)
                return;
            s_rgRec.b.clear();
            s_rgRec.put(static_cast<uint32_t>(i));
            s_rgRec.put(static_cast<uint64_t>(c.hashValid ? c.nextHash : c.ops->blkHash(vu)));
            s_rgRec.put(pc);
            s_rgRec.put(static_cast<uint64_t>(vu.m_cycle));
            s_rgRec.raw(&vu.m_state, sizeof(vu.m_state));
            const uint32_t dsz = c.dataSize;
            s_rgRec.put(dsz);
            s_rgRec.raw(c.vuData, dsz);
            rgPending(s_rgRec, vu, vu.m_cycle);
            s_rgMem0.assign(c.vuData, c.vuData + dsz);
            s_rgRing.b.clear(); // v3: flag rings (oldest first) and the working clip, which the VU1State snapshot does not carry
            for (const vuflags::FlgRing *ring : {&vuflags::s_flgClip[1], &vuflags::s_flgMac[1]})
            {
                s_rgRing.put(static_cast<uint32_t>(ring->count));
                s_rgRing.put(static_cast<uint32_t>(ring->base));
                for (uint32_t k = 0; k < ring->count; ++k)
                {
                    const uint32_t idx = (ring->head + 8u - ring->count + k) & 7u;
                    s_rgRing.put(static_cast<uint64_t>(ring->cyc[idx]));
                    s_rgRing.put(static_cast<uint32_t>(ring->val[idx]));
                }
            }
            s_rgRing.put(static_cast<uint32_t>(vu.m_workingClip));
            s_rgTrace.clear();
            s_rgTrace.push_back(static_cast<uint16_t>(pc));
            s_rgEntryPc = pc;
            s_rgActive = static_cast<int>(i);
            return;
        }
        return;
    }
    const RgRange &r = s_rgRanges[static_cast<size_t>(s_rgActive)];
    if (pc >= r.a && pc < r.b)
    {
        if (s_rgTrace.size() < 4096u)
            s_rgTrace.push_back(static_cast<uint16_t>(pc));
        return;
    }
    const int idx = s_rgActive;
    s_rgActive = -1;
    s_rgRec.put(pc);
    s_rgRec.put(static_cast<uint64_t>(vu.m_cycle));
    s_rgRec.raw(&vu.m_state, sizeof(vu.m_state));
    uint32_t nd = 0;
    const size_t ndAt = s_rgRec.b.size();
    s_rgRec.put(nd);
    for (uint32_t q = 0; q < c.dataSize / 16u; ++q)
        if (std::memcmp(s_rgMem0.data() + q * 16u, c.vuData + q * 16u, 16u) != 0) { s_rgRec.put(q); s_rgRec.raw(c.vuData + q * 16u, 16u); ++nd; }
    std::memcpy(s_rgRec.b.data() + ndAt, &nd, 4);
    rgPending(s_rgRec, vu, vu.m_cycle);
    s_rgRec.put(static_cast<uint64_t>(c.ops->blkHash(vu)));
    const uint32_t nt = static_cast<uint32_t>(s_rgTrace.size());
    s_rgRec.put(nt);
    s_rgRec.raw(s_rgTrace.data(), nt * sizeof(uint16_t));
    s_rgRec.put(static_cast<uint32_t>(0x33565247u)); // 'GRV3' marker, then the entry ring dump
    s_rgRec.raw(s_rgRing.b.data(), s_rgRing.b.size());
    if (!s_rgFile) s_rgFile = std::fopen("logs/vu1regions.bin", "wb");
    if (!s_rgFile) return;
    const uint32_t tag = 0x204D4752u /* 'RGM ' */, bytes = static_cast<uint32_t>(s_rgRec.b.size());
    std::fwrite(&tag, 4, 1, s_rgFile);
    std::fwrite(&bytes, 4, 1, s_rgFile);
    std::fwrite(s_rgRec.b.data(), 1, bytes, s_rgFile);
    std::fflush(s_rgFile);
    ++s_rgRanges[static_cast<size_t>(idx)].byPc[s_rgEntryPc][1];
    ++s_rgRanges[static_cast<size_t>(idx)].kept;
}
}

namespace
{
// PERF-LB (Level B, PS2X_VU1_KERNEL=1): closed-form replacement of the whole skinned-vertex loop block (pc 20F8, steady-state entry hash 11f8...).
// The model, provenance and teleport checks are tools/vu1gen/lbmodel.py, lbprov*.py, lbtele.py (50k teleports over the region corpus, 0 mismatches).
// Entry at boundary p (before pass p), K = (vi8 - vi9) / 4 passes remain; the block's own exit state is boundary b = p + K. Every pass is nine FMACs on
// fixed stage values (vertex t: T5, Q, R7 = T5*Q, S2Q = S2*Q, Wraw, BV = B*vf16, inner, prod = S3*inner) so the whole loop is computed per vertex.
// Declines (returns false, nothing touched) whenever a precondition is not provably the steady state; the real block then runs.
const bool s_lbOn = []() { const char *e = std::getenv("PS2X_VU1_KERNEL"); return e && e[0] == '1'; }();
constexpr uint64_t kLbLoopHash = 0x11f866f1c074d242ull;
uint64_t s_lbTaken = 0, s_lbDeclined = 0, s_lbPasses = 0, s_lbWhy[8] = {};

struct LbOp
{
    __m128 res;
    uint32_t cur, xf, mac;
};

template <int KIND, int DEST>
inline LbOp lbFmac(__m128 a, __m128 b, __m128 ac) // operands as in fmacR (normalised here); result lanes outside DEST are unspecified
{
    constexpr bool madd = KIND == 2 || KIND == 3;
    constexpr uint32_t D4 = vuhot::rev4(static_cast<uint32_t>(DEST));
    const __m128 a4 = vuhot::N4(a), b4 = vuhot::N4(b);
    __m128 ac4 = _mm_setzero_ps();
    __m128 val;
    if constexpr (KIND == 0)
        val = _mm_add_ps(a4, b4);
    else if constexpr (KIND == 1)
        val = _mm_sub_ps(a4, b4);
    else if constexpr (KIND == 4)
        val = _mm_mul_ps(a4, b4);
    else
    {
        ac4 = vuhot::N4(ac);
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
    uint32_t mac, status4, extra = 0u;
    __m128i vb = _mm_castps_si128(val);
    if ((bad & D4) == 0u)
    {
        const uint32_t s = static_cast<uint32_t>(_mm_movemask_ps(val)) & D4;
        mac = vuhot::rev4(s) << 4;
        status4 = s ? 2u : 0u;
        if constexpr (madd)
            extra = (static_cast<uint32_t>(_mm_movemask_ps(prodf)) & D4) ? 2u : 0u;
    }
    else
    {
        uint32_t zl = 0u; // PERF-FZ (as in fmacR): out-of-window lanes whose float result is exactly +-0 and whose exact result is certainly zero
        if constexpr (!madd)
        {
            const uint32_t zv = static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpeq_ps(val, _mm_setzero_ps())));
            uint32_t g = 0xFu;
            if constexpr (KIND == 4)
                g = static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpeq_ps(a4, _mm_setzero_ps()))) |
                    static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpeq_ps(b4, _mm_setzero_ps())));
            zl = zv & g & bad & D4;
        }
        if (!madd && ((bad & D4) & ~zl) == 0u)
        {
            const uint32_t s = static_cast<uint32_t>(_mm_movemask_ps(val)) & D4;
            mac = (vuhot::rev4(s) << 4) | vuhot::rev4(zl);
            status4 = (s ? 2u : 0u) | (zl ? 1u : 0u);
        }
        else
        {
            const vuhot::FmacSlow sl = vuhot::fmacSlow<KIND, DEST>(a4, b4, ac4, val);
            vb = sl.vb;
            mac = sl.mac;
            status4 = sl.status4;
            extra = sl.extra;
        }
    }
    return LbOp{_mm_castsi128_ps(vb), status4 & 0xFu, extra, mac};
}

inline __m128 lbBc(__m128 v, int lane) // broadcast one lane (operand normalisation happens in lbFmac)
{
    switch (lane)
    {
    case 0: return _mm_shuffle_ps(v, v, 0x00);
    case 1: return _mm_shuffle_ps(v, v, 0x55);
    case 2: return _mm_shuffle_ps(v, v, 0xAA);
    default: return _mm_shuffle_ps(v, v, 0xFF);
    }
}

inline __m128 lbClamp(__m128 x, float iReg) // MAXx.xyzw (vf0.x = 0) then MINIi.xyzw
{
    const __m128 a = vuhot::N4(x);
    return _mm_min_ps(_mm_max_ps(a, _mm_setzero_ps()), _mm_set1_ps(vuhot::N(iReg)));
}

template <int SCALE>
inline __m128i lbFtoi(__m128 a) // vuhot::ftoi core
{
    // PERF-FT: SCALE is a power of two, so the float product is exact (or inf, which saturates); cvtt gives INT_MIN for everything out of range, which
    // is already the <= INT_MIN result. Floats are never strictly between 2147483520 and 2^31, so `>= 2^31` equals the old double `>= INT_MAX`.
    const __m128 a4 = vuhot::N4(a);
    const __m128 sc = _mm_mul_ps(a4, _mm_set1_ps(static_cast<float>(SCALE)));
    const __m128i t = _mm_cvttps_epi32(sc);
    const __m128 ge = _mm_cmpge_ps(sc, _mm_set1_ps(2147483648.0f));
    return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(t), _mm_castsi128_ps(_mm_set1_epi32(std::numeric_limits<int32_t>::max())), ge));
}

struct LbV
{
    int32_t idx;
    __m128 A, B, S3, S2, T5, R7, S2Q, inner, BV, prod;
    float Q, Wraw;
    uint32_t di, sticky[8];
    uint32_t macIn, curIn, macR7, curR7;
};

inline int32_t lbS16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

bool lbLoop(VU1Interpreter &vu, VuNativeCtx &c)
{
    VU1State &st = vu.m_state;
    constexpr int kMaxPasses = 64;
    const auto decline = [&](int why) { ++s_lbDeclined; ++s_lbWhy[why]; return false; };
    if (vu.m_xgkick.active || st.branchPending || st.ebit || vu.m_stopRequested || st.haltAfterDelaySlot)
        return decline(0);
    const int32_t vi2 = st.vi[2], vi3 = st.vi[3], vi4 = st.vi[4], vi5 = st.vi[5], vi6 = st.vi[6], vi8 = st.vi[8], vi9 = st.vi[9];
    const int32_t dd = lbS16(vi8 - vi9), d9 = lbS16(vi9 - vi6 - 12);
    if (dd <= 0 || (dd & 3) != 0 || d9 < 0 || (d9 & 3) != 0)
        return decline(1);
    const int32_t K = dd / 4, p = d9 / 4 + 1, b = p + K;
    if (K > kMaxPasses || p < 2 || vi5 != 2 * (b + 2))
        return decline(2);
    if (static_cast<int64_t>(c.budgetEnd) - static_cast<int64_t>(vu.m_cycle) <= 15 * K + 160)
        return decline(3);
    if (!vu.m_fdiv.valid || (vu.m_fdiv.statusDi & 0x30u) != 0u || vu.m_fdiv.readyCycle != vu.m_cycle + 6u)
        return decline(4);
    const uint32_t dsz = c.dataSize;
    const uint8_t *mem = c.vuData;
    if (dsz < 0x4000u || (dsz & (dsz - 1u)) != 0u)
        return decline(5);
    const auto ld32 = [&](uint32_t qw, uint32_t lane) { uint32_t v; std::memcpy(&v, mem + ((qw * 16u) & (dsz - 1u)) + lane * 4u, 4); return v; };
    const auto ldq = [&](uint32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + ((qw * 16u) & (dsz - 1u)))); };

    const __m128 vf1 = _mm_loadu_ps(st.vf[1]), vf2 = _mm_loadu_ps(st.vf[2]), vf3 = _mm_loadu_ps(st.vf[3]), vf4 = _mm_loadu_ps(st.vf[4]), vf16 = _mm_loadu_ps(st.vf[16]);
    const float iReg = st.i;
    const __m128 zero = _mm_setzero_ps(), vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    const int32_t idxBase = vi4 - (p + 2), s3Base = vi3 - (p + 2), s2Base = vi2 - (p + 1);
    const LbOp accw = lbFmac<4, 1>(vf0, lbBc(vf1, 2), zero); // MULAz.w ACC = vf0 * vf1.z

    // stage values of vertices p-1 .. b+1
    LbV V[kMaxPasses + 3];
    const int32_t t0v = p - 1, nV = K + 3;
    for (int i = 0; i < nV; ++i)
    {
        const int32_t t = t0v + i;
        LbV &r = V[i];
        r.idx = lbS16(static_cast<int32_t>(ld32(static_cast<uint32_t>(static_cast<uint16_t>(idxBase + t)), 0)));
        r.A = ldq(static_cast<uint32_t>(636 + r.idx));
        r.B = ldq(static_cast<uint32_t>(716 + r.idx));
        r.S3 = ldq(static_cast<uint32_t>(static_cast<uint16_t>(s3Base + t)));
        r.S2 = ldq(static_cast<uint32_t>(static_cast<uint16_t>(s2Base + t)));
        const LbOp mula = lbFmac<4, 15>(vf3, r.A, zero);
        const LbOp t5 = lbFmac<2, 15>(vf4, lbBc(r.A, 3), mula.res);
        r.T5 = t5.res;
        const float den = vuhot::N(_mm_cvtss_f32(lbBc(r.T5, 3)));
        float q;
        r.di = 0u;
        if (den == 0.0f)
        {
            r.di = 0x20u; // num is 1.0
            q = std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        else
            q = vuhot::normResult(1.0f / den);
        r.Q = vuhot::normResult(q);
        const __m128 qb = _mm_set1_ps(r.Q);
        const LbOp r7 = lbFmac<4, 14>(r.T5, qb, zero);
        const LbOp s2q = lbFmac<4, 14>(r.S2, qb, zero);
        const LbOp w = lbFmac<2, 1>(vf1, qb, accw.res);
        const LbOp bv = lbFmac<4, 15>(r.B, vf16, zero);
        const LbOp in = lbFmac<2, 15>(vf2, vf16, bv.res);
        const LbOp pr = lbFmac<4, 15>(r.S3, in.res, zero);
        r.R7 = r7.res;
        r.S2Q = s2q.res;
        r.Wraw = _mm_cvtss_f32(lbBc(w.res, 3));
        r.BV = bv.res;
        r.inner = in.res;
        r.prod = pr.res;
        r.sticky[0] = mula.cur | mula.xf;
        r.sticky[1] = t5.cur | t5.xf;
        r.sticky[2] = r7.cur | r7.xf;
        r.sticky[3] = s2q.cur | s2q.xf;
        r.sticky[4] = w.cur | w.xf;
        r.sticky[5] = bv.cur | bv.xf;
        r.sticky[6] = in.cur | in.xf;
        r.sticky[7] = pr.cur | pr.xf;
        r.macIn = in.mac;
        r.curIn = in.cur;
    }
    const auto V_ = [&](int32_t t) -> LbV & { return V[t - t0v]; };
    // every DIV that lands inside the range must be free of D/I flags (the landing would set status bits); the pending one (b+1) may carry any
    for (int32_t t = p + 1; t <= b; ++t)
        if (V_(t).di != 0u)
            return decline(6);

    // ---- commit ----
    const uint64_t cycle0 = vu.m_cycle;
    uint32_t status = st.status;
    const uint32_t ubase = static_cast<uint32_t>(static_cast<uint16_t>(vi6));
    const auto putw = [&](int32_t qwOff, int lane, uint32_t v)
    {
        const uint32_t qw = ubase + static_cast<uint32_t>(qwOff);
        std::memcpy(c.vuData + ((qw * 16u) & (dsz - 1u)) + static_cast<uint32_t>(lane) * 4u, &v, 4);
    };
    const uint32_t accwSticky = accw.cur | accw.xf;
    for (int32_t k = p; k < b; ++k)
    {
        const LbV &vm1 = V_(k - 1), &v0 = V_(k), &v1 = V_(k + 1), &v2 = V_(k + 2);
        const uint32_t sticky = v1.sticky[7] | v0.sticky[2] | accwSticky | v1.sticky[3] | v1.sticky[4] | v2.sticky[0] | v2.sticky[1] | v2.sticky[5] | v2.sticky[6];
        status = (status & 0xC00u) | ((status & 0x3C0u) | ((sticky & 0xFu) << 6)) | v2.curIn;
        vuflags::flgPush(vuflags::s_flgMac[1], cycle0 + 15u * static_cast<uint64_t>(k - p) + 11u, v2.macIn);
        alignas(16) uint32_t tmp[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(tmp), lbFtoi<16>(lbClamp(_mm_set1_ps(vm1.Wraw), iReg)));
        putw(4 * k - 3, 3, tmp[0]);
        _mm_store_si128(reinterpret_cast<__m128i *>(tmp), lbFtoi<16>(vm1.R7));
        for (int l = 0; l < 3; ++l) putw(4 * k, l, tmp[l]);
        _mm_store_si128(reinterpret_cast<__m128i *>(tmp), lbFtoi<1>(lbClamp(v0.prod, iReg)));
        for (int l = 0; l < 4; ++l) putw(4 * k + 3, l, tmp[l]);
        _mm_store_si128(reinterpret_cast<__m128i *>(tmp), _mm_castps_si128(v1.S2Q));
        for (int l = 0; l < 3; ++l) putw(4 * k + 6, l, tmp[l]);
        putw(4 * k + 12, 3, static_cast<uint32_t>(v2.idx) & 0xFFFFu);
    }
    status &= ~0x30u; // the pending DIV landed during the first pass; its D/I bits are 0 (checked above)
    const LbV &eM1 = V_(b - 1), &e0 = V_(b), &e1 = V_(b + 1);
    const auto setv = [&](int r, __m128 v) { _mm_storeu_ps(st.vf[r], v); };
    const auto setv3 = [&](int r, __m128 v) { float t4[4]; _mm_storeu_ps(t4, v); st.vf[r][0] = t4[0]; st.vf[r][1] = t4[1]; st.vf[r][2] = t4[2]; };
    setv(5, e1.T5);
    {
        alignas(16) uint32_t t6[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(t6), lbFtoi<16>(eM1.R7));
        uint32_t w6[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(w6), lbFtoi<16>(lbClamp(_mm_set1_ps(eM1.Wraw), iReg)));
        t6[3] = w6[0];
        std::memcpy(st.vf[6], t6, 16);
    }
    {
        const __m128 cw = lbClamp(_mm_set1_ps(eM1.Wraw), iReg);
        setv3(7, eM1.R7);
        st.vf[7][3] = _mm_cvtss_f32(cw);
    }
    setv(8, lbClamp(e0.prod, iReg));
    setv3(9, e0.T5);
    st.vf[9][3] = _mm_cvtss_f32(_mm_max_ps(vuhot::N4(_mm_set1_ps(e0.Wraw)), _mm_setzero_ps()));
    setv(10, e1.S3);
    setv(11, e1.inner);
    setv3(12, e0.S2Q);
    _mm_storeu_ps(st.acc, e1.BV);
    st.q = e0.Q;
    for (int i = 2; i <= 4; ++i) st.vi[i] = lbS16(st.vi[i] + K);
    st.vi[7] = e1.idx;
    st.vi[9] = lbS16(vi9 + 4 * K);
    st.status = status;
    const uint64_t S = cycle0 + 15u * static_cast<uint64_t>(K - 1); // start of the last pass: the block's exit bookkeeping is relative to it
    vu.m_cycle = S + 15u;
    st.cycles = vu.m_cycle;
    vu.m_fdiv.valid = true;
    vu.m_fdiv.readyCycle = S + 21u;
    vu.m_fdiv.value = e1.Q;
    vu.m_fdiv.statusDi = e1.di & 0x30u;
    st.pc = 0x2170u;
    st.branchPending = false;
    vu.m_viBranchBackupValid = false;
    c.instrs += 15u * static_cast<uint32_t>(K);
    *c.nextReadyW = 0u;
    for (int l = 0; l < 4; ++l)
    {
        vu.m_accReady[l] = S + 11u;
        vu.m_vfReady[10][l] = S + 12u;
        vu.m_vfReady[11][l] = S + 15u;
        vu.m_vfReady[5][l] = S + 13u;
        vu.m_vfReady[6][l] = S + 18u;
        vu.m_vfReady[8][l] = S + 16u;
    }
    for (int l = 0; l < 3; ++l)
    {
        vu.m_vfReady[12][l] = S + 10u;
        vu.m_vfReady[7][l] = S + 5u;
        vu.m_vfReady[9][l] = S + 13u;
    }
    vu.m_vfReady[7][3] = S + 7u;
    vu.m_vfReady[9][3] = S + 17u;
    vu.m_viReady[2] = S + 2u;
    vu.m_viReady[3] = S + 9u;
    vu.m_viReady[4] = S + 3u;
    vu.m_viReady[7] = S + 4u;
    vu.m_viReady[9] = S + 12u;
    c.nextHash = kLbLoopHash;
    c.hashValid = true;
    ++s_lbTaken;
    s_lbPasses += static_cast<uint64_t>(K);
    if ((s_lbTaken & 0xFFFFull) == 0u)
        std::printf("[probe] lb kernel taken=%llu passes=%llu declined=%llu (why0-6: %llu %llu %llu %llu %llu %llu %llu)\n", (unsigned long long)s_lbTaken, (unsigned long long)s_lbPasses,
                    (unsigned long long)s_lbDeclined, (unsigned long long)s_lbWhy[0], (unsigned long long)s_lbWhy[1], (unsigned long long)s_lbWhy[2], (unsigned long long)s_lbWhy[3],
                    (unsigned long long)s_lbWhy[4], (unsigned long long)s_lbWhy[5], (unsigned long long)s_lbWhy[6]);
    return true;
}

// PERF-LB full routine (PS2X_VU1_KERNEL=1): the whole skinned-vertex routine 1FE0 -> JR in closed form (prologue block + first pass + loop + tail), N >= 4 vertices.
// Spec and validation: tools/vu1gen/lbfull.py (exit state from the entry state alone, checked against the recorded exit and the exact model).
// Entry contract (all 1500 corpus entries): every pipeline empty, no pending DIV, no running XGKICK. Exit: pc = vi15 * 8 with the XGKICK started by the
// real delay-slot pair, so the last two pairs are executed with the same calls as the tail block.
constexpr uint64_t kLbEntryHash = 0xed9ddb4fd6ac295full, kLbExitHash = 0x1234ABCDull; // PERF-XG: empty token list (the old value was this state plus the transfer-active token, which the hash no longer carries)
uint64_t s_lbFullTaken = 0, s_lbFullDeclined = 0, s_lbFullVerts = 0, s_lbFullWhy[10] = {};
uint64_t s_lb8Groups = 0, s_lb8Fallback = 0; // PERF-SV

struct LbK
{
    __m128 vf1, vf2, vf3, vf4, vf16;
    LbOp accw;
};

inline void lbVertex(LbV &r, int32_t idx, uint32_t s3qw, uint32_t s2qw, const LbK &k, const uint8_t *mem, uint32_t dsz)
{
    const auto ldq = [&](uint32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + ((qw * 16u) & (dsz - 1u)))); };
    const __m128 zero = _mm_setzero_ps();
    r.idx = idx;
    r.A = ldq(static_cast<uint32_t>(636 + idx));
    r.B = ldq(static_cast<uint32_t>(716 + idx));
    r.S3 = ldq(s3qw);
    r.S2 = ldq(s2qw);
    const LbOp mula = lbFmac<4, 15>(k.vf3, r.A, zero);
    const LbOp t5 = lbFmac<2, 15>(k.vf4, lbBc(r.A, 3), mula.res);
    r.T5 = t5.res;
    const float den = vuhot::N(_mm_cvtss_f32(lbBc(r.T5, 3)));
    float q;
    r.di = 0u;
    if (den == 0.0f)
    {
        r.di = 0x20u;
        q = std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    else
        q = vuhot::normResult(1.0f / den);
    r.Q = vuhot::normResult(q);
    const __m128 qb = _mm_set1_ps(r.Q);
    const LbOp r7 = lbFmac<4, 14>(r.T5, qb, zero);
    const LbOp s2q = lbFmac<4, 14>(r.S2, qb, zero);
    const LbOp w = lbFmac<2, 1>(k.vf1, qb, k.accw.res);
    const LbOp bv = lbFmac<4, 15>(r.B, k.vf16, zero);
    const LbOp in = lbFmac<2, 15>(k.vf2, k.vf16, bv.res);
    const LbOp pr = lbFmac<4, 15>(r.S3, in.res, zero);
    r.R7 = r7.res;
    r.S2Q = s2q.res;
    r.Wraw = _mm_cvtss_f32(lbBc(w.res, 3));
    r.BV = bv.res;
    r.inner = in.res;
    r.prod = pr.res;
    r.sticky[0] = mula.cur | mula.xf;
    r.sticky[1] = t5.cur | t5.xf;
    r.sticky[2] = r7.cur | r7.xf;
    r.sticky[3] = s2q.cur | s2q.xf;
    r.sticky[4] = w.cur | w.xf;
    r.sticky[5] = bv.cur | bv.xf;
    r.sticky[6] = in.cur | in.xf;
    r.sticky[7] = pr.cur | pr.xf;
    r.macIn = in.mac;
    r.curIn = in.cur;
    r.macR7 = r7.mac;
    r.curR7 = r7.cur;
}

// PERF-SV: eight vertices per step (struct-of-arrays, AVX2). Produces exactly what lbVertex produces whenever every result lane of every op is either inside the
// exponent window [2,253] or an exactly-certain zero (a zero operand makes a product zero; a madd zero also needs a zero accumulator). Anything else -> returns false
// and the caller runs the scalar lbVertex for those vertices, so the output is never approximated.
struct Lb8V
{
    __m256 c[4];
};

struct Lb8K
{
    Lb8V vf1, vf2, vf3, vf4, vf16, accw;
};

inline __m256 lb8N(__m256 v) // vuhot::N4 on 8 lanes
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

inline __m256i lb8OutWin(__m256 v)
{
    const __m256i ev = _mm256_and_si256(_mm256_srli_epi32(_mm256_castps_si256(v), 23), _mm256_set1_epi32(0xFF));
    return _mm256_or_si256(_mm256_cmpgt_epi32(_mm256_set1_epi32(2), ev), _mm256_cmpgt_epi32(ev, _mm256_set1_epi32(253)));
}

inline __m256i lb8IsZ(__m256 v) { return _mm256_castps_si256(_mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_EQ_OQ)); }

inline bool lb8Any(__m256i m) { return !_mm256_testz_si256(m, m); }

struct Lb8Op
{
    Lb8V r;
    __m256i bad, mac, anyZ, anyS, xZ, xS;
};

template <int DEST, bool MADD>
inline Lb8Op lb8Op(const Lb8V &a, const Lb8V &b, const Lb8V &ac)
{
    Lb8Op o;
    const __m256i z0 = _mm256_setzero_si256();
    o.bad = o.mac = o.anyZ = o.anyS = o.xZ = o.xS = z0;
    for (int c = 0; c < 4; ++c)
    {
        if (((DEST >> (3 - c)) & 1) == 0)
        {
            o.r.c[c] = _mm256_setzero_ps(); // lanes outside DEST are never read
            continue;
        }
        const __m256 av = a.c[c], bv = b.c[c];
        const __m256 val = MADD ? _mm256_fmadd_ps(av, bv, ac.c[c]) : _mm256_mul_ps(av, bv);
        o.r.c[c] = val;
        const __m256i za = _mm256_or_si256(lb8IsZ(av), lb8IsZ(bv)); // the exact product is zero
        __m256i zok = _mm256_and_si256(lb8IsZ(val), za);
        if (MADD)
            zok = _mm256_and_si256(zok, lb8IsZ(ac.c[c]));
        o.bad = _mm256_or_si256(o.bad, _mm256_andnot_si256(zok, lb8OutWin(val)));
        const __m256i sg = _mm256_srai_epi32(_mm256_castps_si256(val), 31);
        o.mac = _mm256_or_si256(o.mac, _mm256_or_si256(_mm256_and_si256(zok, _mm256_set1_epi32(8 >> c)), _mm256_and_si256(sg, _mm256_set1_epi32(0x80 >> c))));
        o.anyZ = _mm256_or_si256(o.anyZ, zok);
        o.anyS = _mm256_or_si256(o.anyS, sg);
        if (MADD)
        {
            const __m256 p = _mm256_mul_ps(av, bv);
            o.bad = _mm256_or_si256(o.bad, _mm256_andnot_si256(za, lb8OutWin(p)));
            o.xZ = _mm256_or_si256(o.xZ, za);
            o.xS = _mm256_or_si256(o.xS, _mm256_srai_epi32(_mm256_castps_si256(p), 31));
        }
    }
    return o;
}

inline uint32_t lb8Sticky(const Lb8Op &o, bool madd) // OR over lanes of (cur | xf)
{
    uint32_t s = (lb8Any(o.anyZ) ? 1u : 0u) | (lb8Any(o.anyS) ? 2u : 0u);
    if (madd)
        s |= (lb8Any(o.xZ) ? 1u : 0u) | (lb8Any(o.xS) ? 2u : 0u);
    return s;
}

inline Lb8V lb8Const(__m128 v)
{
    v = vuhot::N4(v);
    Lb8V r;
    r.c[0] = _mm256_broadcastss_ps(v);
    r.c[1] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0x55));
    r.c[2] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0xAA));
    r.c[3] = _mm256_broadcastss_ps(_mm_shuffle_ps(v, v, 0xFF));
    return r;
}

inline Lb8V lb8Bc(const Lb8V &v, int lane) // one component of every vertex broadcast to all four components
{
    Lb8V r;
    r.c[0] = r.c[1] = r.c[2] = r.c[3] = v.c[lane];
    return r;
}

inline Lb8V lb8FromRows(const __m128 rows[8]) // eight xyzw rows -> four component vectors
{
    const __m256 r0 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[0]), rows[4], 1);
    const __m256 r1 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[1]), rows[5], 1);
    const __m256 r2 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[2]), rows[6], 1);
    const __m256 r3 = _mm256_insertf128_ps(_mm256_castps128_ps256(rows[3]), rows[7], 1);
    const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
    const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
    Lb8V r;
    r.c[0] = _mm256_shuffle_ps(t0, t1, 0x44);
    r.c[1] = _mm256_shuffle_ps(t0, t1, 0xEE);
    r.c[2] = _mm256_shuffle_ps(t2, t3, 0x44);
    r.c[3] = _mm256_shuffle_ps(t2, t3, 0xEE);
    return r;
}

inline void lb8ToRows(const Lb8V &v, __m128 rows[8])
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

// idx / s3q / s2q hold 8 entries (the caller pads a short last group with copies of its last vertex, which cannot change the OR-aggregated flags).
// Fills out[0..n); the stage sticky bits of the whole group are OR-ed into out[0].sticky (lbFull only ever ORs them over all vertices).
inline bool lbVertex8(LbV *out, int n, const int32_t *idx, const uint32_t *s3q, const uint32_t *s2q, const Lb8K &k8, const uint8_t *mem, uint32_t dsz)
{
    const auto ldq = [&](uint32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + ((qw * 16u) & (dsz - 1u)))); };
    __m128 rA[8], rB[8], rS3[8], rS2[8];
    for (int j = 0; j < 8; ++j)
    {
        rA[j] = ldq(static_cast<uint32_t>(636 + idx[j]));
        rB[j] = ldq(static_cast<uint32_t>(716 + idx[j]));
        rS3[j] = ldq(s3q[j]);
        rS2[j] = ldq(s2q[j]);
    }
    Lb8V A = lb8FromRows(rA), B = lb8FromRows(rB), S3 = lb8FromRows(rS3), S2 = lb8FromRows(rS2);
    for (int c = 0; c < 4; ++c)
    {
        A.c[c] = lb8N(A.c[c]);
        B.c[c] = lb8N(B.c[c]);
        S3.c[c] = lb8N(S3.c[c]);
        S2.c[c] = lb8N(S2.c[c]);
    }
    Lb8V zero;
    zero.c[0] = zero.c[1] = zero.c[2] = zero.c[3] = _mm256_setzero_ps();

    const Lb8Op mula = lb8Op<15, false>(k8.vf3, A, zero);
    const Lb8Op t5 = lb8Op<15, true>(k8.vf4, lb8Bc(A, 3), mula.r);
    const __m256 den = lb8N(t5.r.c[3]);
    if (lb8Any(lb8IsZ(den)))
        return false; // the scalar path reports the divide-by-zero
    const __m256 qv = lb8N(lb8N(_mm256_div_ps(_mm256_set1_ps(1.0f), den)));
    Lb8V Q;
    Q.c[0] = Q.c[1] = Q.c[2] = Q.c[3] = qv;
    const Lb8Op r7 = lb8Op<14, false>(t5.r, Q, zero);
    const Lb8Op os2q = lb8Op<14, false>(S2, Q, zero);
    const Lb8Op w = lb8Op<1, true>(k8.vf1, Q, k8.accw);
    const Lb8Op bv = lb8Op<15, false>(B, k8.vf16, zero);
    const Lb8Op in = lb8Op<15, true>(k8.vf2, k8.vf16, bv.r);
    const Lb8Op pr = lb8Op<15, false>(S3, in.r, zero);
    const __m256i bad = _mm256_or_si256(_mm256_or_si256(_mm256_or_si256(mula.bad, t5.bad), _mm256_or_si256(r7.bad, os2q.bad)),
                                        _mm256_or_si256(_mm256_or_si256(w.bad, bv.bad), _mm256_or_si256(in.bad, pr.bad)));
    if (lb8Any(bad))
        return false;

    __m128 rT5[8], rR7[8], rS2Q[8], rIn[8], rBV[8], rPr[8];
    lb8ToRows(t5.r, rT5);
    lb8ToRows(r7.r, rR7);
    lb8ToRows(os2q.r, rS2Q);
    lb8ToRows(in.r, rIn);
    lb8ToRows(bv.r, rBV);
    lb8ToRows(pr.r, rPr);
    alignas(32) float qf[8], wf[8];
    alignas(32) uint32_t macIn[8], macR7[8];
    _mm256_store_ps(qf, qv);
    _mm256_store_ps(wf, w.r.c[3]);
    _mm256_store_si256(reinterpret_cast<__m256i *>(macIn), in.mac);
    _mm256_store_si256(reinterpret_cast<__m256i *>(macR7), r7.mac);
    for (int j = 0; j < n; ++j)
    {
        LbV &r = out[j];
        r.idx = idx[j];
        r.A = rA[j];
        r.B = rB[j];
        r.S3 = rS3[j];
        r.S2 = rS2[j];
        r.T5 = rT5[j];
        r.R7 = rR7[j];
        r.S2Q = rS2Q[j];
        r.inner = rIn[j];
        r.BV = rBV[j];
        r.prod = rPr[j];
        r.Q = qf[j];
        r.Wraw = wf[j];
        r.di = 0u;
        for (int s = 0; s < 8; ++s)
            r.sticky[s] = 0u;
        r.macIn = macIn[j];
        r.curIn = ((macIn[j] & 0xFu) ? 1u : 0u) | ((macIn[j] & 0xF0u) ? 2u : 0u);
        r.macR7 = macR7[j];
        r.curR7 = ((macR7[j] & 0xFu) ? 1u : 0u) | ((macR7[j] & 0xF0u) ? 2u : 0u);
    }
    out[0].sticky[0] = lb8Sticky(mula, false);
    out[0].sticky[1] = lb8Sticky(t5, true);
    out[0].sticky[2] = lb8Sticky(r7, false);
    out[0].sticky[3] = lb8Sticky(os2q, false);
    out[0].sticky[4] = lb8Sticky(w, true);
    out[0].sticky[5] = lb8Sticky(bv, false);
    out[0].sticky[6] = lb8Sticky(in, true);
    out[0].sticky[7] = lb8Sticky(pr, false);
    return true;
}

// PERF-LBR (PS2X_VU1_LBR=1; =2 runs it next to the scalar reference, counts mismatches and keeps using the exact result): relaxed variant of lbVertex8.
// What it relies on: the VU1 microcode never reads the STATUS flags (probe C210b: 0 readers in 134M pairs, none in the image), and reads MAC only for the `in` and
// `r7` results (their words are pushed to the MAC ring), so the other six ops need their VALUES to be exact but not their flags. Values are the same fmadd/mul as
// lb8Op; the exactness test is the same window [2,253] but done as one min/max accumulation per op (zero results are accepted: a cancelling or underflowing result
// is +-0 in the scalar path too, only its flags differ). Also stored: only the rows lbFull reads (S2Q, prod, R7, Wraw, MAC words for every vertex; the other seven
// rows only for the last vertex of the routine).
template <int DEST, bool MADD>
inline Lb8V lb8OpR(const Lb8V &a, const Lb8V &b, const Lb8V &ac, __m256i &minT, __m256i &maxU)
{
    Lb8V r;
    const __m256i absMask = _mm256_set1_epi32(0x7FFFFFFF), one = _mm256_set1_epi32(1);
    for (int c = 0; c < 4; ++c)
    {
        if (((DEST >> (3 - c)) & 1) == 0)
        {
            r.c[c] = _mm256_setzero_ps(); // lanes outside DEST are never read
            continue;
        }
        const __m256 val = MADD ? _mm256_fmadd_ps(a.c[c], b.c[c], ac.c[c]) : _mm256_mul_ps(a.c[c], b.c[c]);
        r.c[c] = val;
        const __m256i u = _mm256_and_si256(_mm256_castps_si256(val), absMask);
        minT = _mm256_min_epu32(minT, _mm256_sub_epi32(u, one)); // u == 0 wraps to 0xFFFFFFFF and never counts as small
        maxU = _mm256_max_epu32(maxU, u);
    }
    return r;
}

uint64_t s_lbrGroups = 0, s_lbrRelaxed = 0, s_lbrDeclined = 0, s_lbrMismatch = 0; // PERF-LBR

inline bool lbVertex8R(LbV *out, int n, const int32_t *idx, const uint32_t *s3q, const uint32_t *s2q, const Lb8K &k8, const uint8_t *mem, uint32_t dsz, uint32_t fullMask)
{
    const auto ldq = [&](uint32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + ((qw * 16u) & (dsz - 1u)))); };
    __m128 rA[8], rB[8], rS3[8], rS2[8];
    for (int j = 0; j < 8; ++j)
    {
        rA[j] = ldq(static_cast<uint32_t>(636 + idx[j]));
        rB[j] = ldq(static_cast<uint32_t>(716 + idx[j]));
        rS3[j] = ldq(s3q[j]);
        rS2[j] = ldq(s2q[j]);
    }
    Lb8V A = lb8FromRows(rA), B = lb8FromRows(rB), S3 = lb8FromRows(rS3), S2 = lb8FromRows(rS2);
    for (int c = 0; c < 4; ++c)
    {
        A.c[c] = lb8N(A.c[c]);
        B.c[c] = lb8N(B.c[c]);
        S3.c[c] = lb8N(S3.c[c]);
        S2.c[c] = lb8N(S2.c[c]);
    }
    Lb8V zero;
    zero.c[0] = zero.c[1] = zero.c[2] = zero.c[3] = _mm256_setzero_ps();
    __m256i minT = _mm256_set1_epi32(-1), maxU = _mm256_setzero_si256();

    const Lb8V mula = lb8OpR<15, false>(k8.vf3, A, zero, minT, maxU);
    const Lb8V t5 = lb8OpR<15, true>(k8.vf4, lb8Bc(A, 3), mula, minT, maxU);
    const __m256 den = lb8N(t5.c[3]);
    if (lb8Any(lb8IsZ(den)))
        return false; // the scalar path reports the divide-by-zero
    const __m256 qv = lb8N(lb8N(_mm256_div_ps(_mm256_set1_ps(1.0f), den)));
    Lb8V Q;
    Q.c[0] = Q.c[1] = Q.c[2] = Q.c[3] = qv;
    const Lb8Op r7 = lb8Op<14, false>(t5, Q, zero); // exact: its MAC word is read
    const Lb8V os2q = lb8OpR<14, false>(S2, Q, zero, minT, maxU);
    const Lb8V w = lb8OpR<1, true>(k8.vf1, Q, k8.accw, minT, maxU);
    const Lb8V bv = lb8OpR<15, false>(B, k8.vf16, zero, minT, maxU);
    const Lb8Op in = lb8Op<15, true>(k8.vf2, k8.vf16, bv); // exact: its MAC word is read
    const Lb8V pr = lb8OpR<15, false>(S3, in.r, zero, minT, maxU);
    const __m256i badLo = _mm256_cmpeq_epi32(_mm256_min_epu32(minT, _mm256_set1_epi32(0x00FFFFFE)), minT); // some result in [1, 0x00FFFFFF]: exponent < 2
    const __m256i badHi = _mm256_cmpgt_epi32(maxU, _mm256_set1_epi32(0x7EFFFFFF));                         // some result with exponent > 253
    if (lb8Any(_mm256_or_si256(_mm256_or_si256(badLo, badHi), _mm256_or_si256(r7.bad, in.bad))))
        return false;

    __m128 rR7[8], rS2Q[8], rPr[8], rT5[8], rIn[8], rBV[8];
    lb8ToRows(r7.r, rR7);
    lb8ToRows(os2q, rS2Q);
    lb8ToRows(pr, rPr);
    if (fullMask != 0u)
    {
        lb8ToRows(t5, rT5);
        lb8ToRows(in.r, rIn);
        lb8ToRows(bv, rBV);
    }
    alignas(32) float qf[8], wf[8];
    alignas(32) uint32_t macIn[8], macR7[8];
    _mm256_store_ps(qf, qv);
    _mm256_store_ps(wf, w.c[3]);
    _mm256_store_si256(reinterpret_cast<__m256i *>(macIn), in.mac);
    _mm256_store_si256(reinterpret_cast<__m256i *>(macR7), r7.mac);
    for (int j = 0; j < n; ++j)
    {
        LbV &r = out[j];
        r.idx = idx[j];
        r.R7 = rR7[j];
        r.S2Q = rS2Q[j];
        r.prod = rPr[j];
        r.Wraw = wf[j];
        r.di = 0u;
        r.macIn = macIn[j];
        r.macR7 = macR7[j];
        r.curR7 = ((macR7[j] & 0xFu) ? 1u : 0u) | ((macR7[j] & 0xF0u) ? 2u : 0u);
        if ((fullMask >> j) & 1u)
        {
            r.A = rA[j];
            r.B = rB[j];
            r.S3 = rS3[j];
            r.S2 = rS2[j];
            r.T5 = rT5[j];
            r.inner = rIn[j];
            r.BV = rBV[j];
            r.Q = qf[j];
            r.curIn = ((macIn[j] & 0xFu) ? 1u : 0u) | ((macIn[j] & 0xF0u) ? 2u : 0u);
        }
    }
    for (int s = 0; s < 8; ++s)
        out[0].sticky[s] = 0u; // STATUS sticky bits are only tracked for the two exact ops; nothing reads STATUS
    out[0].sticky[2] = lb8Sticky(r7, false);
    out[0].sticky[6] = lb8Sticky(in, true);
    return true;
}

inline bool lbSameRow(__m128 a, __m128 b) { return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_castps_si128(a), _mm_castps_si128(b))) == 0xFFFF; }

// PS2X_VU1_LBR=2: compare what lbVertex8R stored with the scalar reference (the fields lbFull reads); returns the number of differing vertices
inline int lbCompareRelaxed(const LbV *rel, const LbV *ref, int n, uint32_t fullMask, int32_t t0)
{
    int bad = 0;
    for (int j = 0; j < n; ++j)
    {
        const LbV &a = rel[j], &b = ref[j];
        bool same = a.idx == b.idx && std::bit_cast<uint32_t>(a.Wraw) == std::bit_cast<uint32_t>(b.Wraw) && lbSameRow(a.S2Q, b.S2Q) && lbSameRow(a.prod, b.prod) &&
                    lbSameRow(a.R7, b.R7) && a.macIn == b.macIn && a.macR7 == b.macR7 && a.curR7 == b.curR7;
        if (same && ((fullMask >> j) & 1u))
            same = lbSameRow(a.A, b.A) && lbSameRow(a.B, b.B) && lbSameRow(a.S3, b.S3) && lbSameRow(a.S2, b.S2) && lbSameRow(a.T5, b.T5) && lbSameRow(a.inner, b.inner) &&
                   lbSameRow(a.BV, b.BV) && std::bit_cast<uint32_t>(a.Q) == std::bit_cast<uint32_t>(b.Q) && a.curIn == b.curIn;
        if (!same)
        {
            if (s_lbrMismatch + bad < 8u)
                std::printf("[probe] lbr MISMATCH vertex %d idx=%d macIn %08X/%08X macR7 %08X/%08X wraw %08X/%08X\n", t0 + j, a.idx, a.macIn, b.macIn, a.macR7, b.macR7,
                            std::bit_cast<uint32_t>(a.Wraw), std::bit_cast<uint32_t>(b.Wraw));
            ++bad;
        }
    }
    return bad;
}

bool lbFull(VU1Interpreter &vu, VuNativeCtx &c)
{
    VU1State &st = vu.m_state;
    constexpr int kMaxV = 67;
    const auto decline = [&](int why) { ++s_lbFullDeclined; ++s_lbFullWhy[why]; return false; };
    if (vu.m_xgkick.active || st.branchPending || st.ebit || vu.m_stopRequested || st.haltAfterDelaySlot)
        return decline(0);
    if (vu.m_fdiv.valid || vu.m_efu[0].valid || vu.m_efu[1].valid || *c.storeMask != 0u)
        return decline(1);
    for (const auto &w : vu.m_vfWritePipeline) if (w.valid) return decline(1);
    for (const auto &w : vu.m_viWritePipeline) if (w.valid) return decline(1);
    for (const auto &w : vu.m_accWritePipeline) if (w.valid) return decline(1);
    for (const auto &w : vu.m_flagPipeline) if (w.valid) return decline(1);
    for (const auto &w : vu.m_storePipeline) if (w.valid) return decline(1);
    const uint32_t dsz = c.dataSize;
    if (dsz != 0x4000u)
        return decline(2);
    const int32_t vi2 = st.vi[2], vi3 = st.vi[3], vi4 = st.vi[4], vi5 = st.vi[5], vi6 = st.vi[6], vi7 = st.vi[7], vi12 = st.vi[12], vi13 = st.vi[13], vi15 = st.vi[15];
    if ((vi5 & 1) != 0 || vi5 < 8 || vi5 / 2 > kMaxV)
        return decline(3);
    const int32_t N = vi5 / 2, K = N - 3;
    if (static_cast<int64_t>(c.budgetEnd) - static_cast<int64_t>(vu.m_cycle) <= 74 + 15 * K + 160)
        return decline(4);
    const uint8_t *mem = c.vuData;
    const auto ld32 = [&](uint32_t qw, uint32_t lane) { uint32_t v; std::memcpy(&v, mem + ((qw * 16u) & (dsz - 1u)) + lane * 4u, 4); return v; };
    const auto ldq = [&](uint32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + ((qw * 16u) & (dsz - 1u)))); };
    const float iReg = 255.0f; // the IMM pair (437F0000) loads I before the first MINIi
    LbK k;
    k.vf1 = ldq(1016);
    k.vf2 = ldq(1014);
    k.vf3 = ldq(1002);
    k.vf4 = ldq(1003);
    k.vf16 = _mm_loadu_ps(st.vf[16]);
    k.accw = lbFmac<4, 1>(_mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), lbBc(k.vf1, 2), _mm_setzero_ps());
    LbV V[kMaxV];
    int32_t idxA[kMaxV];
    for (int32_t t = 0; t < N; ++t)
        idxA[t] = t == 0 ? vi7 : lbS16(static_cast<int32_t>(ld32(static_cast<uint32_t>(static_cast<uint16_t>(vi4 + t)), 0)));
    static const bool s_lb8On = []() { const char *e = std::getenv("PS2X_VU1_LB8"); return !(e && e[0] == '0'); }(); // PERF-SV: =0 forces the scalar vertex loop
    static const int s_lbrMode = []() { const char *e = std::getenv("PS2X_VU1_LBR"); return e && e[0] == '1' ? 1 : (e && e[0] == '2' ? 2 : 0); }(); // PERF-LBR
    uint32_t stickyAcc = k.accw.cur | k.accw.xf;
    Lb8K k8;
    k8.vf1 = lb8Const(k.vf1);
    k8.vf2 = lb8Const(k.vf2);
    k8.vf3 = lb8Const(k.vf3);
    k8.vf4 = lb8Const(k.vf4);
    k8.vf16 = lb8Const(k.vf16);
    k8.accw = lb8Const(k.accw.res);
    for (int32_t t0 = 0; t0 < N; t0 += 8)
    {
        const int n = std::min<int32_t>(8, N - t0);
        bool done = false, relaxed = false;
        if (s_lb8On)
        {
            int32_t idx8[8];
            uint32_t s3q[8], s2q[8];
            for (int j = 0; j < 8; ++j)
            {
                const int jj = j < n ? j : n - 1; // short last group: pad with copies of its last vertex
                idx8[j] = idxA[t0 + jj];
                s3q[j] = static_cast<uint32_t>(static_cast<uint16_t>(vi3 + t0 + jj));
                s2q[j] = static_cast<uint32_t>(static_cast<uint16_t>(vi2 + vi12 + t0 + jj));
            }
            if (s_lbrMode != 0)
            {
                ++s_lbrGroups;
                const uint32_t fullMask = (t0 + n == N) ? (1u << (n - 1)) : 0u; // only the last vertex of the routine needs every intermediate row
                relaxed = lbVertex8R(&V[t0], n, idx8, s3q, s2q, k8, mem, dsz, fullMask);
                if (!relaxed)
                    ++s_lbrDeclined;
                else if (s_lbrMode == 2)
                {
                    LbV ref[8];
                    for (int j = 0; j < n; ++j)
                    {
                        const int32_t t = t0 + j;
                        lbVertex(ref[j], idxA[t], static_cast<uint32_t>(static_cast<uint16_t>(vi3 + t)), static_cast<uint32_t>(static_cast<uint16_t>(vi2 + vi12 + t)), k, mem, dsz);
                    }
                    s_lbrMismatch += static_cast<uint64_t>(lbCompareRelaxed(&V[t0], ref, n, fullMask, t0));
                    for (int j = 0; j < n; ++j)
                        V[t0 + j] = ref[j]; // verify mode keeps using the exact result
                    relaxed = false;
                    done = true;
                }
                else
                    ++s_lbrRelaxed;
            }
            if (!relaxed && !done)
                done = lbVertex8(&V[t0], n, idx8, s3q, s2q, k8, mem, dsz);
        }
        ++s_lb8Groups;
        if (!done && !relaxed)
        {
            ++s_lb8Fallback;
            for (int j = 0; j < n; ++j)
            {
                const int32_t t = t0 + j;
                lbVertex(V[t], idxA[t], static_cast<uint32_t>(static_cast<uint16_t>(vi3 + t)), static_cast<uint32_t>(static_cast<uint16_t>(vi2 + vi12 + t)), k, mem, dsz);
            }
        }
        if (relaxed)
        {
            for (int s = 0; s < 8; ++s) // lbVertex8R puts the group's sticky bits in its first vertex only; the other vertices hold no STATUS data
                stickyAcc |= V[t0].sticky[s];
        }
        else
        {
            for (int j = 0; j < n; ++j)
            {
                if (V[t0 + j].di != 0u)
                    return decline(5);
                for (int s = 0; s < 8; ++s)
                    stickyAcc |= V[t0 + j].sticky[s];
            }
        }
    }
    // everything below only commits (reads of the old memory are done first)
    const int32_t newVi1 = lbS16(static_cast<int32_t>(ld32(static_cast<uint32_t>(static_cast<uint16_t>(vi13)), 0)));
    const int32_t newVi11 = lbS16(static_cast<int32_t>(ld32(1023, 3)));
    const uint32_t ctlOld = ld32(1023, 3) & 0xFFFFu;
    const uint32_t vf27x = std::bit_cast<uint32_t>(st.vf[27][0]), vf27y = std::bit_cast<uint32_t>(st.vf[27][1]);
    const uint32_t m1012z = ld32(1012, 2), m1012w = ld32(1012, 3);
    const uint64_t c0 = vu.m_cycle;
    const uint32_t ubase = static_cast<uint32_t>(static_cast<uint16_t>(vi6));
    const auto putw = [&](uint32_t qw, int lane, uint32_t v) { std::memcpy(c.vuData + ((qw * 16u) & (dsz - 1u)) + static_cast<uint32_t>(lane) * 4u, &v, 4); };
    putw(ubase, 0, vf27x);
    putw(ubase, 1, vf27y);
    putw(ubase, 2, m1012z);
    putw(ubase, 3, m1012w);
    for (int32_t v = 0; v < N; ++v) // PERF-FT: whole-quad stores (blend with the old quad where only some lanes are written)
    {
        const uint32_t q0 = ubase + 1u + 4u * static_cast<uint32_t>(v);
        __m128i *p0 = reinterpret_cast<__m128i *>(c.vuData + ((q0 * 16u) & (dsz - 1u)));
        __m128i *p1 = reinterpret_cast<__m128i *>(c.vuData + (((q0 + 1u) * 16u) & (dsz - 1u)));
        __m128i *p2 = reinterpret_cast<__m128i *>(c.vuData + (((q0 + 2u) * 16u) & (dsz - 1u)));
        __m128i *p3 = reinterpret_cast<__m128i *>(c.vuData + (((q0 + 3u) * 16u) & (dsz - 1u)));
        const int32_t w0 = _mm_cvtsi128_si32(lbFtoi<16>(lbClamp(_mm_set1_ps(V[v].Wraw), iReg)));
        _mm_storeu_si128(p0, _mm_insert_epi32(_mm_loadu_si128(p0), w0, 3));
        _mm_storeu_si128(p1, _mm_castps_si128(_mm_blend_ps(_mm_castsi128_ps(_mm_loadu_si128(p1)), V[v].S2Q, 0x7)));
        _mm_storeu_si128(p2, lbFtoi<1>(lbClamp(V[v].prod, iReg)));
        _mm_storeu_si128(p3, _mm_insert_epi32(lbFtoi<16>(V[v].R7), static_cast<int32_t>(static_cast<uint32_t>(V[v].idx) & 0xFFFFu), 3));
    }
    putw(1023, 2, ctlOld);
    putw(1023, 3, ubase);

    // status: sticky OR over every stage op of every vertex, low 4 = R7 of the last vertex (last FMAC of the tail), bits 4-5 cleared by the last landing
    const uint32_t sticky = stickyAcc;
    const uint32_t en = st.status;
    st.status = (en & 0xC00u) | ((en & 0x3C0u) | ((sticky & 0xFu) << 6)) | V[N - 1].curR7;
    // MAC ring pushes at the KEEPMAC pairs: prologue 20D8 (inner of vertex 2), one per loop pass (inner of k+2), tail 21C0 (R7 of the last vertex)
    vuflags::flgPush(vuflags::s_flgMac[1], c0 + 41u, V[2].macIn);
    for (int32_t p = 1; p <= K; ++p)
        vuflags::flgPush(vuflags::s_flgMac[1], c0 + 45u + 15u * static_cast<uint64_t>(p - 1) + 11u, V[p + 2].macIn);
    vuflags::flgPush(vuflags::s_flgMac[1], c0 + 45u + 15u * static_cast<uint64_t>(K) + 10u, V[N - 1].macR7);

    // vector registers
    const LbV &L0 = V[N - 1], &L1 = V[N - 2], &L2 = V[N - 3];
    const auto setl = [&](int r, __m128 v, int n) { float t4[4]; _mm_storeu_ps(t4, v); for (int l = 0; l < n; ++l) st.vf[r][l] = t4[l]; };
    const auto clampW = [&](float w) { return lbClamp(_mm_set1_ps(w), iReg); };
    setl(1, L0.T5, 3);
    st.vf[1][3] = std::bit_cast<float>(ld32(1016, 3));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(st.vf[2]), lbFtoi<1>(lbClamp(L1.prod, iReg)));
    setl(3, L0.S2, 3);
    st.vf[3][3] = std::bit_cast<float>(ld32(1002, 3));
    setl(4, L0.S2Q, 3);
    st.vf[4][3] = _mm_cvtss_f32(lbBc(L0.prod, 3));
    {
        uint32_t t5[4], w5[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(t5), lbFtoi<16>(L1.R7));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(w5), lbFtoi<16>(clampW(L1.Wraw)));
        t5[3] = w5[0];
        std::memcpy(st.vf[5], t5, 16);
        uint32_t t6[4], w6[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(t6), lbFtoi<16>(L0.R7));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(w6), lbFtoi<16>(clampW(L0.Wraw)));
        t6[3] = w6[0];
        std::memcpy(st.vf[6], t6, 16);
    }
    setl(7, L2.R7, 3);
    st.vf[7][3] = _mm_cvtss_f32(clampW(L2.Wraw));
    _mm_storeu_ps(st.vf[8], lbClamp(L0.prod, iReg));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(st.vf[9]), lbFtoi<1>(lbClamp(L0.prod, iReg)));
    _mm_storeu_ps(st.vf[10], L0.S3);
    _mm_storeu_ps(st.vf[11], L0.inner);
    setl(12, L1.S2Q, 3);
    st.vf[27][2] = std::bit_cast<float>(m1012z);
    st.vf[27][3] = std::bit_cast<float>(m1012w);
    {
        float a4[4];
        _mm_storeu_ps(a4, L0.BV);
        st.acc[0] = a4[0];
        st.acc[1] = a4[1];
        st.acc[2] = a4[2];
        st.acc[3] = _mm_cvtss_f32(lbBc(k.accw.res, 3));
    }
    st.q = L0.Q;
    st.i = iReg;
    st.vi[1] = newVi1;
    st.vi[2] = lbS16(vi2 + vi12 + N);
    st.vi[3] = lbS16(vi3 + N);
    st.vi[4] = lbS16(vi4 + N);
    st.vi[7] = L0.idx;
    st.vi[8] = lbS16(vi6 + 2 * vi5);
    st.vi[9] = st.vi[8];
    st.vi[11] = newVi11;
    st.vi[12] = V[1].idx;

    // timing: prologue 45 cycles, one first-pass block (15), K-1 loop passes (15 each), then the tail (29 cycles): its last two pairs run for real
    const uint64_t Sp = c0, S1 = c0 + 45u, Sl = c0 + 45u + 15u * static_cast<uint64_t>(K > 1 ? K - 1 : 0), St = c0 + 45u + 15u * static_cast<uint64_t>(K);
    const VuNativeOps &op = *c.ops;
    vu.m_cycle = St + 27u;
    st.pc = 0x21F8u;
    vu.m_viBranchBackupValid = false;
    { // pair 17 @ 21F8 (JR vi15)
        const uint32_t jt = (static_cast<uint32_t>(static_cast<uint16_t>(vuhot::readBranchVi(vu, 15))) * 8u) & 0x3FFFu;
        (void)vi15;
        st.branchPending = true; st.branchTarget = jt; st.branchDelay = 1;
        vu.m_viBranchBackupValid = false;
        st.pc = 0x2200u;
        if (st.branchPending)
        {
            if (st.branchDelay == 0u) { st.pc = st.branchTarget & 0x3FFFu; st.branchPending = false; }
            else { --st.branchDelay; }
        }
    }
    ++vu.m_cycle;
    if (vu.m_xgkick.active) op.xgProgress(vu);
    { // pair 18 @ 2200 (XGKICK vi6 in the delay slot)
        st.pc = 0x2200u;
        op.execLower(vu, 0x800036fcu, c.vuData, c.dataSize, *c.gs, c.memory, 0x000002ffu);
        vu.m_viBranchBackupValid = false;
        st.vf[0][0] = 0.0f; st.vf[0][1] = 0.0f; st.vf[0][2] = 0.0f; st.vf[0][3] = 1.0f; st.vi[0] = 0;
        st.pc = 0x2208u;
        if (st.branchPending)
        {
            if (st.branchDelay == 0u) { st.pc = st.branchTarget & 0x3FFFu; st.branchPending = false; }
            else { --st.branchDelay; }
        }
    }
    op.advance1(vu);
    st.cycles = vu.m_cycle;
    // ready times exactly as the chain of real blocks leaves them (later blocks overwrite earlier ones)
    vu.m_accReady[0] = Sp + 41u;
    vu.m_accReady[1] = Sp + 41u;
    vu.m_accReady[2] = Sp + 41u;
    vu.m_accReady[3] = Sp + 41u;
    vu.m_vfReady[10][0] = Sp + 42u;
    vu.m_vfReady[10][1] = Sp + 42u;
    vu.m_vfReady[10][2] = Sp + 42u;
    vu.m_vfReady[10][3] = Sp + 42u;
    vu.m_vfReady[11][0] = Sp + 45u;
    vu.m_vfReady[11][1] = Sp + 45u;
    vu.m_vfReady[11][2] = Sp + 45u;
    vu.m_vfReady[11][3] = Sp + 45u;
    vu.m_vfReady[1][2] = Sp + 9u;
    vu.m_vfReady[1][3] = Sp + 9u;
    vu.m_vfReady[27][3] = Sp + 41u;
    vu.m_vfReady[2][0] = Sp + 8u;
    vu.m_vfReady[2][1] = Sp + 8u;
    vu.m_vfReady[2][2] = Sp + 8u;
    vu.m_vfReady[2][3] = Sp + 8u;
    vu.m_vfReady[3][0] = Sp + 5u;
    vu.m_vfReady[3][1] = Sp + 5u;
    vu.m_vfReady[3][2] = Sp + 5u;
    vu.m_vfReady[3][3] = Sp + 5u;
    vu.m_vfReady[4][0] = Sp + 6u;
    vu.m_vfReady[4][1] = Sp + 6u;
    vu.m_vfReady[4][2] = Sp + 6u;
    vu.m_vfReady[4][3] = Sp + 6u;
    vu.m_vfReady[5][0] = Sp + 43u;
    vu.m_vfReady[5][1] = Sp + 43u;
    vu.m_vfReady[5][2] = Sp + 43u;
    vu.m_vfReady[5][3] = Sp + 43u;
    vu.m_vfReady[6][0] = Sp + 46u;
    vu.m_vfReady[6][1] = Sp + 46u;
    vu.m_vfReady[6][2] = Sp + 46u;
    vu.m_vfReady[6][3] = Sp + 46u;
    vu.m_vfReady[7][0] = Sp + 40u;
    vu.m_vfReady[7][1] = Sp + 40u;
    vu.m_vfReady[7][2] = Sp + 40u;
    vu.m_vfReady[8][0] = Sp + 47u;
    vu.m_vfReady[8][1] = Sp + 47u;
    vu.m_vfReady[8][2] = Sp + 47u;
    vu.m_vfReady[8][3] = Sp + 47u;
    vu.m_vfReady[9][0] = Sp + 43u;
    vu.m_vfReady[9][1] = Sp + 43u;
    vu.m_vfReady[9][2] = Sp + 43u;
    vu.m_vfReady[9][3] = Sp + 48u;
    vu.m_viReady[12] = Sp + 19u;
    vu.m_viReady[2] = Sp + 33u;
    vu.m_viReady[3] = Sp + 39u;
    vu.m_viReady[4] = Sp + 43u;
    vu.m_viReady[7] = Sp + 34u;
    vu.m_viReady[8] = Sp + 34u;
    vu.m_viReady[9] = Sp + 42u;
    vu.m_accReady[0] = S1 + 11u;
    vu.m_accReady[1] = S1 + 11u;
    vu.m_accReady[2] = S1 + 11u;
    vu.m_accReady[3] = S1 + 11u;
    vu.m_vfReady[10][0] = S1 + 12u;
    vu.m_vfReady[10][1] = S1 + 12u;
    vu.m_vfReady[10][2] = S1 + 12u;
    vu.m_vfReady[10][3] = S1 + 12u;
    vu.m_vfReady[11][0] = S1 + 15u;
    vu.m_vfReady[11][1] = S1 + 15u;
    vu.m_vfReady[11][2] = S1 + 15u;
    vu.m_vfReady[11][3] = S1 + 15u;
    vu.m_vfReady[12][0] = S1 + 10u;
    vu.m_vfReady[12][1] = S1 + 10u;
    vu.m_vfReady[12][2] = S1 + 10u;
    vu.m_vfReady[5][0] = S1 + 13u;
    vu.m_vfReady[5][1] = S1 + 13u;
    vu.m_vfReady[5][2] = S1 + 13u;
    vu.m_vfReady[5][3] = S1 + 13u;
    vu.m_vfReady[6][0] = S1 + 18u;
    vu.m_vfReady[6][1] = S1 + 18u;
    vu.m_vfReady[6][2] = S1 + 18u;
    vu.m_vfReady[6][3] = S1 + 18u;
    vu.m_vfReady[7][0] = S1 + 5u;
    vu.m_vfReady[7][1] = S1 + 5u;
    vu.m_vfReady[7][2] = S1 + 5u;
    vu.m_vfReady[7][3] = S1 + 7u;
    vu.m_vfReady[8][0] = S1 + 16u;
    vu.m_vfReady[8][1] = S1 + 16u;
    vu.m_vfReady[8][2] = S1 + 16u;
    vu.m_vfReady[8][3] = S1 + 16u;
    vu.m_vfReady[9][0] = S1 + 13u;
    vu.m_vfReady[9][1] = S1 + 13u;
    vu.m_vfReady[9][2] = S1 + 13u;
    vu.m_vfReady[9][3] = S1 + 17u;
    vu.m_viReady[2] = S1 + 2u;
    vu.m_viReady[3] = S1 + 9u;
    vu.m_viReady[4] = S1 + 3u;
    vu.m_viReady[7] = S1 + 4u;
    vu.m_viReady[9] = S1 + 12u;
    if (K >= 2)
    {
        vu.m_accReady[0] = Sl + 11u;
        vu.m_accReady[1] = Sl + 11u;
        vu.m_accReady[2] = Sl + 11u;
        vu.m_accReady[3] = Sl + 11u;
        vu.m_vfReady[10][0] = Sl + 12u;
        vu.m_vfReady[10][1] = Sl + 12u;
        vu.m_vfReady[10][2] = Sl + 12u;
        vu.m_vfReady[10][3] = Sl + 12u;
        vu.m_vfReady[11][0] = Sl + 15u;
        vu.m_vfReady[11][1] = Sl + 15u;
        vu.m_vfReady[11][2] = Sl + 15u;
        vu.m_vfReady[11][3] = Sl + 15u;
        vu.m_vfReady[12][0] = Sl + 10u;
        vu.m_vfReady[12][1] = Sl + 10u;
        vu.m_vfReady[12][2] = Sl + 10u;
        vu.m_vfReady[5][0] = Sl + 13u;
        vu.m_vfReady[5][1] = Sl + 13u;
        vu.m_vfReady[5][2] = Sl + 13u;
        vu.m_vfReady[5][3] = Sl + 13u;
        vu.m_vfReady[6][0] = Sl + 18u;
        vu.m_vfReady[6][1] = Sl + 18u;
        vu.m_vfReady[6][2] = Sl + 18u;
        vu.m_vfReady[6][3] = Sl + 18u;
        vu.m_vfReady[7][0] = Sl + 5u;
        vu.m_vfReady[7][1] = Sl + 5u;
        vu.m_vfReady[7][2] = Sl + 5u;
        vu.m_vfReady[7][3] = Sl + 7u;
        vu.m_vfReady[8][0] = Sl + 16u;
        vu.m_vfReady[8][1] = Sl + 16u;
        vu.m_vfReady[8][2] = Sl + 16u;
        vu.m_vfReady[8][3] = Sl + 16u;
        vu.m_vfReady[9][0] = Sl + 13u;
        vu.m_vfReady[9][1] = Sl + 13u;
        vu.m_vfReady[9][2] = Sl + 13u;
        vu.m_vfReady[9][3] = Sl + 17u;
        vu.m_viReady[2] = Sl + 2u;
        vu.m_viReady[3] = Sl + 9u;
        vu.m_viReady[4] = Sl + 3u;
        vu.m_viReady[7] = Sl + 4u;
        vu.m_viReady[9] = Sl + 12u;
    }
    vu.m_accReady[3] = St + 5u;
    vu.m_vfReady[1][0] = St + 8u;
    vu.m_vfReady[1][1] = St + 8u;
    vu.m_vfReady[1][2] = St + 8u;
    vu.m_vfReady[27][2] = St + 5u;
    vu.m_vfReady[27][3] = St + 5u;
    vu.m_vfReady[2][0] = St + 6u;
    vu.m_vfReady[2][1] = St + 6u;
    vu.m_vfReady[2][2] = St + 6u;
    vu.m_vfReady[2][3] = St + 6u;
    vu.m_vfReady[3][0] = St + 4u;
    vu.m_vfReady[3][1] = St + 4u;
    vu.m_vfReady[3][2] = St + 4u;
    vu.m_vfReady[4][0] = St + 10u;
    vu.m_vfReady[4][1] = St + 10u;
    vu.m_vfReady[4][2] = St + 10u;
    vu.m_vfReady[4][3] = St + 4u;
    vu.m_vfReady[5][0] = St + 12u;
    vu.m_vfReady[5][1] = St + 12u;
    vu.m_vfReady[5][2] = St + 12u;
    vu.m_vfReady[5][3] = St + 12u;
    vu.m_vfReady[6][0] = St + 25u;
    vu.m_vfReady[6][1] = St + 25u;
    vu.m_vfReady[6][2] = St + 25u;
    vu.m_vfReady[6][3] = St + 25u;
    vu.m_vfReady[8][0] = St + 13u;
    vu.m_vfReady[8][1] = St + 13u;
    vu.m_vfReady[8][2] = St + 13u;
    vu.m_vfReady[8][3] = St + 13u;
    vu.m_vfReady[9][0] = St + 17u;
    vu.m_vfReady[9][1] = St + 17u;
    vu.m_vfReady[9][2] = St + 17u;
    vu.m_vfReady[9][3] = St + 17u;
    vu.m_viReady[11] = St + 6u;
    vu.m_viReady[1] = St + 25u;
    vu.m_viReady[2] = St + 1u;
    c.instrs += 54u + 15u * static_cast<uint32_t>(K);
    *c.nextReadyW = 0u;
    c.nextHash = kLbExitHash;
    c.hashValid = true;
    ++s_lbFullTaken;
    s_lbFullVerts += static_cast<uint64_t>(N);
    if ((s_lbFullTaken & 0xFFFFull) == 0u)
        std::printf("[probe] lb full kernel taken=%llu vertices=%llu declined=%llu (why0-5: %llu %llu %llu %llu %llu %llu)\n", (unsigned long long)s_lbFullTaken, (unsigned long long)s_lbFullVerts,
                    (unsigned long long)s_lbFullDeclined, (unsigned long long)s_lbFullWhy[0], (unsigned long long)s_lbFullWhy[1], (unsigned long long)s_lbFullWhy[2], (unsigned long long)s_lbFullWhy[3],
                    (unsigned long long)s_lbFullWhy[4], (unsigned long long)s_lbFullWhy[5]);
    if ((s_lbFullTaken & 0xFFFFull) == 0u)
        std::printf("[probe] lb8 groups=%llu scalar_fallback=%llu\n", (unsigned long long)s_lb8Groups, (unsigned long long)s_lb8Fallback); // PERF-SV
    if (s_lbrMode != 0)
        std::printf("[probe] lbr mode=%d groups=%llu relaxed=%llu declined=%llu mismatched=%llu\n", s_lbrMode, (unsigned long long)s_lbrGroups, (unsigned long long)s_lbrRelaxed,
                    (unsigned long long)s_lbrDeclined, (unsigned long long)s_lbrMismatch); // PERF-LBR
    return true;
}

// PERF-LB handler 0C80 (PS2X_VU1_KERNEL=1): the stream -> VU memory copy command. n = vi2 - vi3 quads are copied from the command stream (vi13) to vi3 (forward, one
// quad at a time exactly like the loop), vf1 = the last quad, vi3 = vi2, vi13 += n, vi1 = the next command address loaded after the copy, JR vi15. Cycles: 5n + 5.
// Spec and check: tools/vu1gen/h_copy.py (900 records, 0 mismatches). Replaces blocks vuH_10 (first iteration), vuH_4 (loop), vuH_17 (JR tail).
constexpr uint64_t kHashCopyEntry = 0xcd057af54d65447full, kHashCopyExit = 0x8aba1b92c58909d9ull;
uint64_t s_hCopyTaken = 0, s_hCopyDeclined = 0, s_hCopyQuads = 0;

bool lbPipesEmpty(VU1Interpreter &vu, VuNativeCtx &c)
{
    if (vu.m_fdiv.valid || vu.m_efu[0].valid || vu.m_efu[1].valid || *c.storeMask != 0u)
        return false;
    for (const auto &w : vu.m_vfWritePipeline) if (w.valid) return false;
    for (const auto &w : vu.m_viWritePipeline) if (w.valid) return false;
    for (const auto &w : vu.m_accWritePipeline) if (w.valid) return false;
    for (const auto &w : vu.m_flagPipeline) if (w.valid) return false;
    for (const auto &w : vu.m_storePipeline) if (w.valid) return false;
    return true;
}

// PERF-LB-XG (PS2X_VU1_XG_HOT=1): kernels may run while an XGKICK transfer is in flight. The transfer copies one VU-memory quadword every 2 cycles
// (VU1Interpreter::progressXgkick, called once per cycle), so running the kernel first and then advancing the transfer by the kernel's cycle count is exact
// when nothing the kernel writes lies in the part of the packet that has not been copied yet. That remaining extent is found by walking the GIF tags from the
// current copy position (tags not yet copied are read from memory the kernel must not touch). Anything unusual declines.
struct LbXg
{
    bool act = false;
    uint32_t a = 0, len = 0; // remaining source bytes of the running transfer, circular in VU data memory
};
uint64_t s_xgK[2] = {}; // kernel runs completed with a transfer in flight, declines because the kernel writes into the remaining extent

inline bool lbXgBegin(VU1Interpreter &vu, VuNativeCtx &c, LbXg &x)
{
    x = LbXg{};
    if (!vu.m_xgkick.active)
        return true;
    if (!s_xgHot || c.dataSize != 0x4000u)
        return false;
    const auto &g = vu.m_xgkick;
    const uint32_t mask = c.dataSize - 1u;
    uint32_t pos = g.copiedBytes, tagEnd = g.currentTagEnd;
    bool eop = g.currentTagEop;
    for (int guard = 0; guard < 64; ++guard)
    {
        if (tagEnd == 0u)
        {
            uint64_t tagLo;
            std::memcpy(&tagLo, c.vuData + ((g.sourceAddress + pos) & mask), 8);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu), format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
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
                return false;
            if (pos > g.packet.size() || tagBytes > g.packet.size() - pos)
                return false;
            tagEnd = pos + static_cast<uint32_t>(tagBytes);
            eop = ((tagLo >> 15) & 1u) != 0u;
        }
        if (eop)
        {
            x.act = true;
            x.a = (g.sourceAddress + g.copiedBytes) & mask;
            x.len = tagEnd - g.copiedBytes;
            return true;
        }
        pos = tagEnd;
        tagEnd = 0u;
    }
    return false;
}

inline bool lbXgOverlap(const LbXg &x, uint32_t b, uint32_t blen, uint32_t mask) // does [b, b + blen) (circular) touch the remaining extent?
{
    if (!x.act || x.len == 0u || blen == 0u)
        return false;
    if (x.len > mask || blen > mask)
        return true;
    b &= mask;
    return ((b - x.a) & mask) < x.len || ((x.a - b) & mask) < blen;
}

inline void lbXgEnd(VU1Interpreter &vu, VuNativeCtx &c, const LbXg &x, uint64_t cycles) // advance the transfer by the cycles the kernel consumed
{
    if (!x.act || cycles == 0u)
        return;
    vu.m_xgkick.cycleCredit += static_cast<decltype(vu.m_xgkick.cycleCredit)>(cycles - 1u);
    c.ops->xgProgress(vu); // adds the last cycle and copies / finishes
    ++s_xgK[0];
}

bool lbCopy(VU1Interpreter &vu, VuNativeCtx &c)
{
    VU1State &st = vu.m_state;
    const uint32_t dsz = c.dataSize;
    LbXg xg;
    if (st.branchPending || st.ebit || vu.m_stopRequested || st.haltAfterDelaySlot || dsz != 0x4000u || !lbPipesEmpty(vu, c) || !lbXgBegin(vu, c, xg))
    {
        ++s_hCopyDeclined;
        return false;
    }
    const int32_t vi2 = st.vi[2], vi13 = st.vi[13], vi15 = st.vi[15];
    const auto qwAt = [&](int32_t qw) { return (static_cast<uint32_t>(qw) * 16u) & (dsz - 1u); };
    uint32_t zw;
    std::memcpy(&zw, c.vuData + qwAt(vi13 - 1) + 8u, 4); // ILW.z vi3, -1(vi13)
    const int32_t vi3 = lbS16(static_cast<int32_t>(zw & 0xFFFFu));
    const int32_t n = (vi2 - vi3) & 0xFFFF;
    if (n == 0 || n > 1024 || static_cast<int64_t>(c.budgetEnd) - static_cast<int64_t>(vu.m_cycle) <= 5 * n + 5 + 160)
    {
        ++s_hCopyDeclined;
        return false;
    }
    if (lbXgOverlap(xg, qwAt(vi3), static_cast<uint32_t>(n) * 16u, dsz - 1u))
    {
        ++s_hCopyDeclined;
        ++s_xgK[1];
        return false;
    }
    for (int32_t i = 0; i < n; ++i) // forward, one quad per iteration: load then store, as LQI / SQ do
    {
        uint8_t q[16];
        std::memcpy(q, c.vuData + qwAt(static_cast<int32_t>(static_cast<uint16_t>(vi13 + i))), 16);
        std::memcpy(c.vuData + qwAt(static_cast<int32_t>(static_cast<uint16_t>(vi3 + i))), q, 16);
        if (i == n - 1)
            std::memcpy(st.vf[1], q, 16);
    }
    const int32_t newVi13 = lbS16(vi13 + n);
    uint32_t nx;
    std::memcpy(&nx, c.vuData + qwAt(static_cast<int32_t>(static_cast<uint16_t>(newVi13))), 4); // ILW.x vi1, 0(vi13) in the JR delay slot
    st.vi[3] = vi2;
    st.vi[13] = newVi13;
    st.vi[1] = lbS16(static_cast<int32_t>(nx & 0xFFFFu));
    const uint64_t Sp = vu.m_cycle, Sl = Sp + 8u + 5u * static_cast<uint64_t>(n >= 2 ? n - 2 : 0), St = Sp + 8u + 5u * static_cast<uint64_t>(n - 1);
    vu.m_cycle = St + 2u;
    st.cycles = vu.m_cycle;
    st.pc = (static_cast<uint32_t>(static_cast<uint16_t>(vi15)) * 8u) & 0x3FFFu; // JR vi15 (the delay slot has run)
    st.branchPending = false;
    st.branchTarget = st.pc; // the interpreter leaves the landed JR target here, with the delay counter at 0
    st.branchDelay = 0u;
    vu.m_viBranchBackupValid = false;
    vu.m_vfReady[1][0] = Sp + 5u;
    vu.m_vfReady[1][1] = Sp + 5u;
    vu.m_vfReady[1][2] = Sp + 5u;
    vu.m_vfReady[1][3] = Sp + 5u;
    vu.m_viReady[13] = Sp + 2u;
    vu.m_viReady[3] = Sp + 5u;
    if (n >= 2)
    {
        vu.m_vfReady[1][0] = Sl + 4u;
        vu.m_vfReady[1][1] = Sl + 4u;
        vu.m_vfReady[1][2] = Sl + 4u;
        vu.m_vfReady[1][3] = Sl + 4u;
        vu.m_viReady[13] = Sl + 1u;
        vu.m_viReady[3] = Sl + 2u;
    }
    vu.m_viReady[1] = St + 5u;
    c.instrs += 6u + 5u * static_cast<uint32_t>(n - 1) + 2u;
    lbXgEnd(vu, c, xg, vu.m_cycle - Sp);
    *c.nextReadyW = 0u;
    c.nextHash = kHashCopyExit;
    c.hashValid = true;
    ++s_hCopyTaken;
    s_hCopyQuads += static_cast<uint64_t>(n);
    if ((s_hCopyTaken & 0xFFFFFull) == 0u)
        std::printf("[probe] lb copy handler taken=%llu quads=%llu declined=%llu\n", (unsigned long long)s_hCopyTaken, (unsigned long long)s_hCopyQuads, (unsigned long long)s_hCopyDeclined);
    return true;
}

// PERF-LB handler 2440 (PS2X_VU1_KERNEL=1): the per-vertex transform / clip / assemble routine (vertex loop 24C8..25B0 with the trivial-reject chain 28A0..28F8).
// Python spec and checks: tools/vu1gen/vspec.py (control, memory, integer regs, STATUS, vf / ACC / Q / I, clip: exact on every non-clipping record),
// vcyc.py (cycles = 25 + 27 A + 28 B + 35 C0 + 41 C3 + 43 C4 + 45 C5; C1 / C2 unseen -> declined). Runs on a private copy of VU memory (the routine works in place and
// can reach real clipping at 2900+, which is declined without side effects) and stops at the epilogue entry 25B8, whose hot block does XTOP / the 1023 swap / XGKICK.
constexpr uint64_t kHashVertEntry = 0xb4f4b1b4da29db30ull, kHashVertExit = 0xc946c511275567c5ull;
uint64_t s_vTaken = 0, s_vDeclined = 0, s_vVerts = 0, s_vWhy[8] = {};

inline uint32_t lbClipBits(__m128 v)
{
    uint32_t w[4];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(w), _mm_castps_si128(v));
    const int32_t limit = (w[3] & 0x7F800000u) != 0u ? static_cast<int32_t>(w[3] & 0x7FFFFFFFu) : 0x007FFFFF;
    uint32_t fl = 0u;
    for (int k = 0; k < 3; ++k)
    {
        int32_t o0 = static_cast<int32_t>(w[k]);
        int32_t o1 = static_cast<int32_t>(w[k] ^ 0x80000000u);
        if (o0 > limit) fl |= 1u << (2 * k);
        if (o1 > limit) fl |= 1u << (2 * k + 1);
    }
    return fl;
}

inline float lbDivQ(__m128 t, uint32_t &di) // DIV Q, vf0.w, T.w : 1.0 / den with the VU's zero-divisor result
{
    const float den = vuhot::N(_mm_cvtss_f32(lbBc(t, 3)));
    float q;
    di = 0u;
    if (den == 0.0f)
    {
        di = 0x20u;
        q = std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    else
        q = vuhot::normResult(1.0f / den);
    return vuhot::normResult(q);
}

bool lbVert(VU1Interpreter &vu, VuNativeCtx &c)
{
    VU1State &st = vu.m_state;
    constexpr int kMaxN = 300;
    const auto decline = [&](int why) { ++s_vDeclined; ++s_vWhy[why]; return false; };
    const uint32_t dsz = c.dataSize;
    LbXg xg;
    if (st.branchPending || st.ebit || vu.m_stopRequested || st.haltAfterDelaySlot || dsz != 0x4000u || !lbPipesEmpty(vu, c) || !lbXgBegin(vu, c, xg))
        return decline(0);
    const int32_t N = st.vi[5];
    if (N < 1 || N > kMaxN)
        return decline(1);
    if (static_cast<int64_t>(c.budgetEnd) - static_cast<int64_t>(vu.m_cycle) <= 45 * static_cast<int64_t>(N) + 200)
        return decline(2);

    alignas(16) uint8_t mem[0x4000];
    std::memcpy(mem, c.vuData, 0x4000);
    const auto qa = [](int32_t qw) { return (static_cast<uint32_t>(qw) * 16u) & 0x3FFFu; };
    const auto ldw = [&](int32_t qw, int lane) { uint32_t v; std::memcpy(&v, mem + qa(qw) + 4u * static_cast<uint32_t>(lane), 4); return v; };
    const auto ldq = [&](int32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + qa(qw))); };
    const auto putw = [&](int32_t qw, int lane, uint32_t v) { std::memcpy(mem + qa(qw) + 4u * static_cast<uint32_t>(lane), &v, 4); };
    const auto putq = [&](int32_t qw, __m128 v) { _mm_storeu_ps(reinterpret_cast<float *>(mem + qa(qw)), v); };

    const __m128 c1v = ldq(1002), c2v = ldq(1003), c5v = ldq(1016), c14v = ldq(1014), q1012 = ldq(1012);
    const __m128 vf16 = _mm_loadu_ps(st.vf[16]);
    const __m128 zero = _mm_setzero_ps(), vf0 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f), one4 = _mm_set1_ps(1.0f), v2048 = _mm_set1_ps(2048.0f);
    const float iReg = std::bit_cast<float>(0x437F0000u);
    const auto V7 = [&](const __m128 &s) { return _mm_blend_ps(s, v2048, 8); };

    const uint64_t c0 = vu.m_cycle;
    uint32_t sticky = st.status & 0xFC0u, lastCur = 0u;
    const auto acc = [&](const LbOp &o) { sticky |= (o.cur | o.xf) << 6; lastCur = o.cur; };

    __m128 Ps[kMaxN + 2], As[kMaxN + 2], Ss[kMaxN + 2], Cs[kMaxN + 2], Tv[kMaxN + 2], Lit[kMaxN + 2];
    float Qf[kMaxN + 2];
    uint32_t Di[kMaxN + 2], Fb[kMaxN + 2];
    int32_t Wd[kMaxN + 2];
    char Pth[kMaxN + 2];
    int Hit[kMaxN + 2];

    int32_t vi1 = 0, vi2 = lbS16(st.vi[2] + st.vi[12]), vi3 = st.vi[3], vi4 = st.vi[4], vi5 = N, vi6 = st.vi[6], vi8 = -2, vi9 = -2, vi10 = 0;
    const int32_t vi4_0 = vi4;
    Wd[0] = lbS16(st.vi[7]);
    const int32_t ix0 = Wd[0] & 0x7FFF;
    uint32_t wclip = vu.m_workingClip;
    uint64_t pendingPush[2 * kMaxN + 8][2]; // (cycle, value)
    int nPush = 0;
    uint64_t macPush[4 * kMaxN + 8][2]; // MAC ring pushes at the KEEPMAC pairs: 24B8 24C0 (prologue), 24D8 2588 25A8 25B0 (per iteration)
    int nMac = 0;
    const auto macP = [&](uint64_t cyc, uint32_t m) { macPush[nMac][0] = cyc; macPush[nMac][1] = m; ++nMac; };
    const auto clipPush = [&](uint64_t cyc, uint32_t bits) { wclip = ((wclip << 6) | (bits & 0x3Fu)) & 0xFFFFFFu; pendingPush[nPush][0] = cyc; pendingPush[nPush][1] = wclip; ++nPush; };

    // ---- prologue (pairs 2440..24C0): P0, header store, C0, A0, S0
    Ps[0] = ldq(636 + ix0);
    {
        float h[4] = {st.vf[27][0], st.vf[27][1], 0.0f, 0.0f};
        std::memcpy(&h[2], reinterpret_cast<const uint8_t *>(&q1012) + 8, 8);
        putq(vi6, _mm_loadu_ps(h));
    }
    vi6 = lbS16(vi6 + 1);
    Cs[0] = ldq(vi3);
    int32_t pC = lbS16(vi3 + 1), pS = vi2;
    As[0] = ldq(716 + ix0);
    Ss[0] = ldq(pS);
    pS = lbS16(pS + 1);
    Fb[0] = lbClipBits(Ps[0]);
    clipPush(c0 + 9u, Fb[0]);
    vi1 = (Fb[0] & 0x3Fu) ? 1 : 0;
    {
        const LbOp p1 = lbFmac<4, 15>(c1v, Ps[0], zero);
        acc(p1);
        const LbOp p2 = lbFmac<2, 15>(c2v, lbBc(Ps[0], 3), p1.res);
        acc(p2);
        Tv[0] = p2.res;
        const LbOp p3 = lbFmac<4, 15>(Cs[0], vf16, zero);
        acc(p3);
        acc(lbFmac<4, 1>(one4, v2048, zero));
        const LbOp p5 = lbFmac<4, 15>(c14v, p3.res, zero);
        acc(p5);
        macP(c0 + 15u, p5.mac);
        const LbOp p6 = lbFmac<2, 15>(As[0], p3.res, p5.res);
        acc(p6);
        macP(c0 + 16u, p6.mac);
        Lit[0] = p6.res;
        Qf[0] = lbDivQ(Tv[0], Di[0]);
    }

    // ---- vertex loop
    uint64_t cur = c0 + 17u;
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
                if ((vis | kMasks[m]) == 0xFFFFFFu) { hit = m; break; }
            if (hit < 0)
                return decline(3); // real clipping (2900+)
            // reject at test 1 / 2: cost 35 + 2 m is extrapolated from m = 0, 3, 4, 5 (never seen in the capture); the verifier compares cycles exactly
            if (hit == 5)
                putw(1022, 0, static_cast<uint32_t>(vi5)); // ISW.x vi5 in the delay slot of the IBNE at 28F8
        }
        Pth[j] = pth;
        Hit[j] = hit;
        const uint32_t cost = pth == 'A' ? 27u : (pth == 'B' ? 28u : 35u + 2u * static_cast<uint32_t>(hit));
        const uint64_t at24F8 = cur + cost - 24u;

        // vertex j outputs (closed form); flags of every FMAC pair of this iteration, collected in program order
        const __m128 qb = _mm_set1_ps(Qf[j]);
        if (pth != 'A')
        {
            const LbOp f1 = lbFmac<1, 1>(one4, one4, zero);                             // 24D8 SUB.w vf15
            acc(f1);
            macP(cur + 2u, f1.mac);
        }
        const LbOp f2 = lbFmac<4, 1>(vf0, lbBc(c5v, 2), zero);                         // 24F8 MULAz.w
        acc(f2);
        const LbOp f3 = lbFmac<2, 1>(lbBc(c5v, 3), qb, f2.res);                        // 2500 MADDq.w
        acc(f3);
        const __m128 v31 = it == 1 ? _mm_loadu_ps(st.vf[31]) : V7(Ss[it - 2]);
        acc(lbFmac<0, 15>(v31, zero, zero));                                            // 2508 ADDz
        const LbOp f5 = lbFmac<4, 14>(Tv[j], qb, zero);                                 // 2510 MULq.xyz
        acc(f5);
        const LbOp f6 = lbFmac<4, 14>(V7(Ss[j]), qb, zero);                             // 2518 MULq.xyz
        acc(f6);

        // loads / stores in program order
        Wd[it] = lbS16(static_cast<int32_t>(ldw(vi4_0 + it, 0) & 0xFFFFu));
        const int32_t ix = Wd[it] & 0x7FFF;
        Ps[it] = ldq(636 + ix);
        vi6 = lbS16(vi6 + 4);
        {
            float s3[4];
            _mm_storeu_ps(s3, f6.res);
            for (int l = 0; l < 3; ++l)
                putw(vi6 - 3, l, std::bit_cast<uint32_t>(s3[l]));
        }
        As[it] = ldq(716 + ix);
        Ss[it] = ldq(pS);
        pS = lbS16(pS + 1);
        Cs[it] = ldq(pC);
        pC = lbS16(pC + 1);
        {
            const LbOp f7 = lbFmac<4, 15>(c1v, Ps[it], zero);                           // 2550 MULA
            acc(f7);
            const __m128 tw = lbClamp(f3.res, iReg);
            const float vf15w = pth == 'B' ? 0.0f : 2048.0f;
            const LbOp f8 = lbFmac<0, 1>(tw, _mm_set1_ps(vf15w), zero);                // 2560 ADD.w
            acc(f8);
            const LbOp f9 = lbFmac<2, 15>(c2v, lbBc(Ps[it], 3), f7.res);               // 2568 MADDw
            acc(f9);
            Tv[it] = f9.res;
            const __m128 v12 = it == 1 ? _mm_loadu_ps(st.vf[12]) : lbClamp(Lit[it - 2], iReg);
            acc(lbFmac<0, 15>(v12, zero, zero));                                         // 2570 ADDx
            const LbOp f11 = lbFmac<4, 15>(Cs[it], vf16, zero);                         // 2588 MUL
            acc(f11);
            macP(at24F8 + 18u, f11.mac);
            const LbOp f12 = lbFmac<4, 15>(c14v, f11.res, zero);                        // 25A8 MULA
            acc(f12);
            macP(at24F8 + 22u, f12.mac);
            const LbOp f13 = lbFmac<2, 15>(As[it], f11.res, f12.res);                   // 25B0 MADD
            acc(f13);
            macP(at24F8 + 23u, f13.mac);
            Lit[it] = f13.res;
            // outputs of vertex j: RGBA, XYZ (x2) with the w lane = ftoi4(clamp + adc)
            alignas(16) uint32_t rg[4], xy[4], ww[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(rg), lbFtoi<1>(lbClamp(Lit[j], iReg)));
            _mm_store_si128(reinterpret_cast<__m128i *>(xy), lbFtoi<16>(f5.res));
            _mm_store_si128(reinterpret_cast<__m128i *>(ww), lbFtoi<16>(f8.res));
            xy[3] = ww[3];
            for (int l = 0; l < 4; ++l)
            {
                putw(vi6 - 2, l, rg[l]);
                putw(vi6 - 4, l, xy[l]);
                putw(vi6 - 1, l, xy[l]);
            }
        }
        Fb[it] = lbClipBits(Ps[it]);
        {
            float e29[4];
            std::memcpy(e29, st.vf[29], 16);
            const uint32_t v28bits = it == 1 ? lbClipBits(_mm_loadu_ps(e29)) : Fb[it - 2];
            clipPush(at24F8 + 6u, v28bits);
            clipPush(at24F8 + 7u, Fb[it - 1]);
            clipPush(at24F8 + 10u, Fb[it]);
        }
        Qf[it] = lbDivQ(Tv[it], Di[it]);
        vi1 = (Fb[it] & 0x3Fu) ? 1 : 0;
        vi8 = lbS16(vi9 + vi10 + vi1);
        vi9 = vi10;
        vi5 = lbS16(vi5 - 1);
        vi4 = lbS16(vi4 + 1);
        cur += cost;
    }

    // ---- commit (nothing above touched the VU state)
    if (xg.act) // a transfer is running: the quadwords this call changes must lie outside the part of the packet not yet copied
    {
        int32_t lo = -1, hi = -1;
        for (int32_t q = 0; q < 1024; ++q)
            if (std::memcmp(mem + q * 16, c.vuData + q * 16, 16) != 0)
            {
                if (lo < 0)
                    lo = q;
                hi = q;
            }
        if (lo >= 0 && lbXgOverlap(xg, static_cast<uint32_t>(lo) * 16u, static_cast<uint32_t>(hi - lo + 1) * 16u, 0x3FFFu))
        {
            ++s_xgK[1];
            return decline(4);
        }
    }
    std::memcpy(c.vuData, mem, 0x4000);
    const int32_t j = N - 1;
    static const bool vdbg = []() { const char *e = std::getenv("PS2X_VU1_VDBG"); return e && e[0] == '1'; }();
    if (vdbg && s_vTaken < 600)
    {
        char buf[1400];
        int n = std::snprintf(buf, sizeof(buf), "[probe] vdbg call=%llu N=%d vi6=%d paths:", (unsigned long long)s_vTaken, N, st.vi[6]);
        for (int32_t k = 0; k < N && n < 1200; ++k)
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %c%d/%04X/f%u", Pth[k], Hit[k], static_cast<unsigned>(Wd[k]) & 0xFFFFu, (Fb[k] & 0x3Fu) ? 1u : 0u);
        std::printf("%s\n", buf);
    }
    {
        const __m128 e = _mm_loadu_ps(st.vf[0]); // keep vf0
        (void)e;
        const auto setv = [&](int r, __m128 v) { _mm_storeu_ps(st.vf[r], v); };
        const auto setl = [&](int r, __m128 v, int mask) { float t4[4]; _mm_storeu_ps(t4, v); for (int l = 0; l < 4; ++l) if (mask & (1 << l)) st.vf[r][l] = t4[l]; };
        const __m128 qb1 = _mm_set1_ps(Qf[j]);
        const LbOp f5 = lbFmac<4, 14>(Tv[j], qb1, zero);
        const LbOp f6 = lbFmac<4, 14>(V7(Ss[j]), qb1, zero);
        const LbOp f2 = lbFmac<4, 1>(vf0, lbBc(c5v, 2), zero);
        const LbOp f3 = lbFmac<2, 1>(lbBc(c5v, 3), qb1, f2.res);
        const char pth = Pth[j];
        const LbOp f8 = lbFmac<0, 1>(lbClamp(f3.res, iReg), _mm_set1_ps(pth == 'B' ? 0.0f : 2048.0f), zero);
        alignas(16) uint32_t xy[4], ww[4], rg[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(xy), lbFtoi<16>(f5.res));
        _mm_store_si128(reinterpret_cast<__m128i *>(ww), lbFtoi<16>(f8.res));
        _mm_store_si128(reinterpret_cast<__m128i *>(rg), lbFtoi<1>(lbClamp(Lit[j], iReg)));
        xy[3] = ww[3];
        setv(1, c1v);
        setv(2, c2v);
        setl(5, c5v, 0xC);
        setv(14, c14v);
        setl(27, q1012, 0xC);
        setv(6, Tv[N]);
        setv(7, V7(Ss[N]));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(st.vf[8]), _mm_load_si128(reinterpret_cast<const __m128i *>(xy)));
        setv(9, As[N]);
        setv(10, Ps[N]);
        const __m128 zero4 = zero;
        const __m128 v12n = N == 1 ? _mm_loadu_ps(st.vf[12]) : lbClamp(Lit[N - 2], iReg); // vf12 as read by the ADDx of the last iteration
        setv(11, lbFmac<0, 15>(v12n, zero4, zero4).res);
        setv(12, lbClamp(Lit[j], iReg));
        setv(13, Lit[N]);
        st.vf[15][3] = pth == 'B' ? 0.0f : 2048.0f;
        setl(17, f6.res, 0x7);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(st.vf[18]), _mm_load_si128(reinterpret_cast<const __m128i *>(rg)));
        const __m128 v31n = N == 1 ? _mm_loadu_ps(st.vf[31]) : V7(Ss[N - 2]);
        const __m128 e29v = _mm_loadu_ps(st.vf[29]);
        setv(28, N == 1 ? e29v : Ps[N - 2]);
        setv(29, Ps[N - 1]);
        setv(30, lbFmac<0, 15>(v31n, zero4, zero4).res);
        setv(31, V7(Ss[N - 1]));
        const __m128 clN = lbFmac<4, 15>(Cs[N], vf16, zero).res;
        _mm_storeu_ps(st.acc, lbFmac<4, 15>(c14v, clN, zero).res);
    }
    for (int32_t k = 0; k < N; ++k)
        sticky |= Di[k] << 6;
    st.status = (sticky & 0xFC0u) | lastCur | Di[N - 1];
    st.q = Qf[N - 1];
    st.i = iReg;
    st.vi[1] = vi1;
    st.vi[2] = pS;
    st.vi[3] = pC;
    st.vi[4] = vi4;
    st.vi[5] = vi5;
    st.vi[6] = vi6;
    st.vi[7] = Wd[N];
    st.vi[8] = vi8;
    st.vi[9] = vi9;
    st.vi[10] = vi10;
    st.vi[11] = 0x7FFF;
    st.vi[12] = Wd[N] & 0x7FFF;
    for (int k = 0; k < nPush; ++k)
        vuflags::flgPush(vuflags::s_flgClip[1], pendingPush[k][0], static_cast<uint32_t>(pendingPush[k][1]));
    for (int k = 0; k < nMac; ++k)
        vuflags::flgPush(vuflags::s_flgMac[1], macPush[k][0], static_cast<uint32_t>(macPush[k][1]));
    vu.m_workingClip = wclip;
    st.clip = wclip;

    const uint64_t SA = cur - 24u;
    vu.m_cycle = cur;
    st.cycles = cur;
    vu.m_fdiv.valid = true;
    vu.m_fdiv.readyCycle = SA + 26u;
    vu.m_fdiv.value = Qf[N];
    vu.m_fdiv.statusDi = Di[N] & 0x30u;
    st.pc = 0x25B8u;
    st.branchPending = false;
    st.branchDelay = 0u;
    st.branchTarget = Pth[j] == 'A' ? 0x24F0u : 0x24F8u;
    vu.m_viBranchBackupValid = false;
    vu.m_accReady[0] = SA + 23u;
    vu.m_accReady[1] = SA + 23u;
    vu.m_accReady[2] = SA + 23u;
    vu.m_accReady[3] = SA + 23u;
    vu.m_vfReady[10][0] = SA + 9u;
    vu.m_vfReady[10][1] = SA + 9u;
    vu.m_vfReady[10][2] = SA + 9u;
    vu.m_vfReady[10][3] = SA + 9u;
    vu.m_vfReady[11][0] = SA + 19u;
    vu.m_vfReady[11][1] = SA + 19u;
    vu.m_vfReady[11][2] = SA + 19u;
    vu.m_vfReady[11][3] = SA + 19u;
    vu.m_vfReady[12][0] = SA + 20u;
    vu.m_vfReady[12][1] = SA + 20u;
    vu.m_vfReady[12][2] = SA + 20u;
    vu.m_vfReady[12][3] = SA + 20u;
    vu.m_vfReady[13][0] = SA + 27u;
    vu.m_vfReady[13][1] = SA + 27u;
    vu.m_vfReady[13][2] = SA + 27u;
    vu.m_vfReady[13][3] = SA + 27u;
    vu.m_vfReady[17][0] = SA + 8u;
    vu.m_vfReady[17][1] = SA + 8u;
    vu.m_vfReady[17][2] = SA + 8u;
    vu.m_vfReady[18][0] = SA + 20u;
    vu.m_vfReady[18][1] = SA + 20u;
    vu.m_vfReady[18][2] = SA + 20u;
    vu.m_vfReady[18][3] = SA + 20u;
    vu.m_vfReady[28][0] = SA + 5u;
    vu.m_vfReady[28][1] = SA + 5u;
    vu.m_vfReady[28][2] = SA + 5u;
    vu.m_vfReady[28][3] = SA + 5u;
    vu.m_vfReady[29][0] = SA + 6u;
    vu.m_vfReady[29][1] = SA + 6u;
    vu.m_vfReady[29][2] = SA + 6u;
    vu.m_vfReady[29][3] = SA + 6u;
    vu.m_vfReady[30][0] = SA + 6u;
    vu.m_vfReady[30][1] = SA + 6u;
    vu.m_vfReady[30][2] = SA + 6u;
    vu.m_vfReady[30][3] = SA + 6u;
    vu.m_vfReady[31][0] = SA + 14u;
    vu.m_vfReady[31][1] = SA + 14u;
    vu.m_vfReady[31][2] = SA + 14u;
    vu.m_vfReady[31][3] = SA + 14u;
    vu.m_vfReady[6][0] = SA + 18u;
    vu.m_vfReady[6][1] = SA + 18u;
    vu.m_vfReady[6][2] = SA + 18u;
    vu.m_vfReady[6][3] = SA + 18u;
    vu.m_vfReady[7][0] = SA + 15u;
    vu.m_vfReady[7][1] = SA + 15u;
    vu.m_vfReady[7][2] = SA + 15u;
    vu.m_vfReady[8][0] = SA + 21u;
    vu.m_vfReady[8][1] = SA + 21u;
    vu.m_vfReady[8][2] = SA + 21u;
    vu.m_vfReady[8][3] = SA + 21u;
    vu.m_vfReady[9][0] = SA + 13u;
    vu.m_vfReady[9][1] = SA + 13u;
    vu.m_vfReady[9][2] = SA + 13u;
    vu.m_vfReady[9][3] = SA + 13u;
    vu.m_viReady[12] = SA + 5u;
    vu.m_viReady[1] = SA + 15u;
    vu.m_viReady[2] = SA + 12u;
    vu.m_viReady[3] = SA + 14u;
    vu.m_viReady[4] = SA + 7u;
    vu.m_viReady[5] = SA + 19u;
    vu.m_viReady[6] = SA + 8u;
    vu.m_viReady[7] = SA + 4u;
    vu.m_viReady[8] = SA + 16u;
    vu.m_viReady[9] = SA + 18u;
    c.instrs += static_cast<uint32_t>(cur - c0);
    lbXgEnd(vu, c, xg, cur - c0);
    *c.nextReadyW = 0u;
    c.nextHash = kHashVertExit;
    c.hashValid = true;
    ++s_vTaken;
    s_vVerts += static_cast<uint64_t>(N);
    if ((s_vTaken & 0x3FFFu) == 0u)
        std::printf("[probe] lb vertex routine taken=%llu verts=%llu declined=%llu (why0-4: %llu %llu %llu %llu %llu)\n", (unsigned long long)s_vTaken, (unsigned long long)s_vVerts,
                    (unsigned long long)s_vDeclined, (unsigned long long)s_vWhy[0], (unsigned long long)s_vWhy[1], (unsigned long long)s_vWhy[2], (unsigned long long)s_vWhy[3], (unsigned long long)s_vWhy[4]);
    return true;
}
} // namespace

uint64_t *vu1NativeStats(uint32_t which) { return which == 0u ? s_cause : (which == 2u ? s_kTicks : (which == 3u ? s_kCalls : (which == 4u ? s_pcT[0] : (which == 5u ? s_pcN[0] : (which == 6u ? s_pcT[1] : (which == 7u ? s_pcN[1] : (which == 8u ? &s_fused : (which == 9u ? s_xgK : (which >= 10u && which <= 12u ? s_causePc[which - 10u] : (which == 13u ? reinterpret_cast<uint64_t *>(s_entryHash) : (which == 14u ? reinterpret_cast<uint64_t *>(&s_entryHashN) : s_tsc))))))))))); }

template <class F>
static inline bool lbTimed(uint32_t k, F &&f)
{
    if (!s_tscOn)
        return f();
    const uint64_t t0 = __rdtsc();
    const bool r = f();
    s_kTicks[k] += __rdtsc() - t0;
    s_kCalls[k] += r ? 1u : 0u;
    return r;
}
uint32_t vu1CoverageCount() { return s_svCov; } // PROBE-CAPTURE

// PERF-VAR: other microcode images (same program family, a few dozen pairs differ) reuse the main image's executors, hot blocks and kernels wherever
// their pair + usage record is identical (tools/vu1gen/variant_masks.py); every other pc falls back to the exact interpreter (return 2).
#include "generated/vu1_variants.inc"
extern int (*const kVu1Step_t4020[2048])(VU1Interpreter &, VuNativeCtx &); // generated/vu1_native_exec_t4020.cpp: executors for the pairs where image 4020... differs from main
extern const bool kVu1HotStart_t4020[2048];                                  // generated/vu1_hot_blocks_t4020.cpp
VuHotFn vu1HotLookup_t4020(uint32_t pc, uint64_t entryHash);
VuHotFn vu1HotLookupXg_t4020(uint32_t pc, uint64_t entryHash);
namespace
{
const uint8_t *s_same = nullptr;     // null = main image (everything allowed)
bool s_hotOk[2048];                  // hot block starting at this pair is usable (all pairs of its span are identical)
bool s_isT4020 = false;                                       // the image with its own executors / hot blocks for the differing pairs
HotSlot s_hotSlotT[2048] = {}, s_hotSlotTX[2048] = {};
bool s_kOk[3] = {true, true, true};  // kernels: vertex (2440..25B8), copy (0C80..0CB8), full/loop (1FE0..243F)
bool varRange(uint32_t lo, uint32_t hiIncl)
{
    for (uint32_t pc = lo; pc <= hiIncl; pc += 8u)
        if (!s_same[pc >> 3])
            return false;
    return true;
}
} // namespace
void vu1NativeSelectImage(uint64_t hash)
{
    static uint64_t s_lastHash = 0; // PERF-VAR: the decode cache is rebuilt on every microcode upload; only redo the masks (and log) when the image changes
    if (hash == s_lastHash)
        return;
    s_lastHash = hash;
    s_same = nullptr;
    s_isT4020 = (hash == 0x4020410124328442ull);
    for (uint32_t i = 0; i < kVu1VariantCount; ++i)
        if (kVu1Variants[i].hash == hash)
            s_same = kVu1Variants[i].same;
    s_kOk[0] = s_kOk[1] = s_kOk[2] = true;
    if (!s_same)
        return;
    for (uint32_t i = 0; i < 2048u; ++i)
    {
        bool ok = true;
        if (kVu1HotStart[i])
        {
            const uint32_t n = (kVu1HotSpan[i] ? kVu1HotSpan[i] : 1u) + 1u; // +1 pair of margin
            for (uint32_t k = 0; k < n && ok; ++k)
                ok = (i + k) < 2048u && s_same[i + k];
        }
        s_hotOk[i] = ok;
    }
    s_kOk[0] = varRange(0x2440u, 0x25B8u);
    s_kOk[1] = varRange(0x0C80u, 0x0CB8u);
    s_kOk[2] = varRange(0x1FE0u, 0x2438u);
    std::printf("[probe] vu1 variant image %016llx: same pairs %u, hot starts usable %u, kernels vertex=%d copy=%d full=%d\n",
                (unsigned long long)hash, (unsigned)std::count(s_same, s_same + 2048, uint8_t(1)),
                (unsigned)std::count(s_hotOk, s_hotOk + 2048, true), (int)s_kOk[0], (int)s_kOk[1], (int)s_kOk[2]);
}
bool vu1NativeSupportsImage(uint64_t hash)
{
    if (hash == kVu1NativeMainHash)
        return true;
    for (uint32_t i = 0; i < kVu1VariantCount; ++i)
        if (kVu1Variants[i].hash == hash)
            return true;
    return false;
}
