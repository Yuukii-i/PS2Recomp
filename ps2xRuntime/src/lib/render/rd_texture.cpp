// FEAT-RD3: Decode GS textures from CPU VRAM and cache by content hash.
#include "raylib.h"
#include "rlgl.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio> // FEAT-RD3b
#include <filesystem> // FEAT-RD3b
#include <unordered_set> // FEAT-RD3b
#include <mutex> // FEAT-RD3c
#include <cstring> // FEAT-RD3c
#include <unordered_map>
#include <vector>
#include <nmmintrin.h> // FEAT-RD5d

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD3
std::atomic<uint32_t> g_rdKickSerial{0};     // FEAT-RD5
uint32_t g_rdPageGen[512];                   // FEAT-RD5b VRAM 8 KB page write generations (GS thread)
std::atomic<uint32_t> g_rdNoteCalls{0};      // FEAT-RD5b
std::atomic<uint32_t> g_rdFullDecodes{0};    // FEAT-RD5b
uint32_t g_rdClutGen = 0;                    // FEAT-RD5c bumped on every CLUT load (GS thread)
std::atomic<uint32_t> g_rdRawHits{0};        // FEAT-RD5d
std::atomic<uint32_t> g_rdFastHits{0};     // FEAT-RD5c
void ps2xRdMarkVramWrite(uint32_t firstByte, uint32_t byteCount) // FEAT-RD5b
{
    if (byteCount == 0u || firstByte >= 0x400000u)
        return;
    const uint64_t end = std::min<uint64_t>(static_cast<uint64_t>(firstByte) + byteCount, 0x400000ull);
    for (uint32_t p = firstByte / 8192u; p <= static_cast<uint32_t>((end - 1u) / 8192u) && p < 512u; ++p)
        ++g_rdPageGen[p];
}

namespace
{
std::atomic<const uint8_t *> g_rdVram{nullptr}; // FEAT-RD3

constexpr uint32_t kRdRingSize = 65536u; // FEAT-RD5
struct RdSerialSlot                      // FEAT-RD5
{
    std::atomic<uint32_t> serial{0};
    std::atomic<uint64_t> contentId{0};
};
RdSerialSlot g_serialRing[kRdRingSize]; // FEAT-RD5

struct RdContentEntry // FEAT-RD5
{
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba;
};
std::mutex g_contentMutex;                                   // FEAT-RD5
std::unordered_map<uint64_t, RdContentEntry> g_contentMap;   // FEAT-RD5

struct RdContentGl // FEAT-RD5
{
    unsigned int glId = 0;
    uint32_t lastFrame = 0;
};
std::unordered_map<uint64_t, RdContentGl> g_glByContentId; // FEAT-RD5 (main thread only)
uint32_t g_texFramesSinceLog = 0;                          // FEAT-RD5
uint32_t g_exactSinceLog = 0;                              // FEAT-RD5
uint32_t g_fallbackSinceLog = 0;                           // FEAT-RD5

struct RdTexKey // FEAT-RD3
{
    uint64_t tex0;
    uint64_t texa;
    bool operator==(const RdTexKey &o) const = default;
};

uint64_t fnv1a64(const void *data, size_t len, uint64_t seed = 14695981039346656037ull) // FEAT-RD3
{
    const auto *p = static_cast<const uint8_t *>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

struct RdTexKeyHash // FEAT-RD3
{
    size_t operator()(const RdTexKey &k) const noexcept
    {
        uint64_t buf[2] = {k.tex0, k.texa};
        return static_cast<size_t>(fnv1a64(buf, sizeof(buf)));
    }
};

struct RdTexEntry // FEAT-RD3
{
    unsigned int glId = 0;
    uint64_t contentHash = 0;
    uint32_t lastFrame = 0;
    uint32_t lastCheckFrame = 0;
    uint64_t clutHash = 0; // FEAT-RD3c
};

struct RdClutKey // FEAT-RD3c
{
    uint32_t tbp0, tbw, psm, tw, th;
    bool operator==(const RdClutKey &o) const = default;
};

struct RdClutKeyHash // FEAT-RD3c
{
    size_t operator()(const RdClutKey &k) const noexcept { return static_cast<size_t>(fnv1a64(&k, sizeof(k))); }
};

struct RdClutNote // FEAT-RD3c
{
    uint16_t clut[512];
    uint32_t cpsm;
    uint32_t csa;
    uint64_t clutHash;
};

std::mutex g_clutMutex;                                                   // FEAT-RD3c
std::unordered_map<RdClutKey, RdClutNote, RdClutKeyHash> g_clutNotes;     // FEAT-RD3c
uint32_t g_notedClutSinceLog = 0;                                         // FEAT-RD3c
uint32_t g_fallbackClutSinceLog = 0;                                      // FEAT-RD3c

std::unordered_map<RdTexKey, RdTexEntry, RdTexKeyHash> g_texCache; // FEAT-RD3
uint32_t g_texFrame = 1;                                           // FEAT-RD3
uint32_t g_decodesSinceLog = 0;                                    // FEAT-RD3
uint32_t g_unsupportedSinceLog = 0;                                // FEAT-RD3
auto g_lastTexLog = std::chrono::steady_clock::now();              // FEAT-RD3

uint32_t rgba5551ToRgba8888(uint16_t c) // FEAT-RD3
{
    const uint32_t r = (static_cast<uint32_t>(c >> 0) & 0x1Fu) << 3;
    const uint32_t g = (static_cast<uint32_t>(c >> 5) & 0x1Fu) << 3;
    const uint32_t b = (static_cast<uint32_t>(c >> 10) & 0x1Fu) << 3;
    const uint32_t a = (static_cast<uint32_t>(c >> 15) & 0x01u) << 7;
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t applyTexa(uint64_t texa, uint8_t psm, uint32_t texel) // FEAT-RD3
{
    if (psm == GS_PSM_CT32)
        return texel;

    const uint8_t ta0 = static_cast<uint8_t>(texa & 0xFFull);
    const bool aem = ((texa >> 15) & 1ull) != 0ull;
    const uint8_t ta1 = static_cast<uint8_t>((texa >> 32) & 0xFFull);
    const bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

    switch (psm)
    {
    case GS_PSM_CT24:
        a = (aem && rgbZero) ? 0u : ta0;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        if ((a & 0x80u) != 0u)
            a = ta1;
        else
            a = (aem && rgbZero) ? 0u : ta0;
        break;
    default:
        break;
    }
    return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

uint32_t swizzleClutIndexCSM1(uint32_t index) // FEAT-RD3
{
    return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
}

bool isIndexedPsm(uint8_t psm) // FEAT-RD3
{
    return psm == GS_PSM_T8 || psm == GS_PSM_T8H ||
           psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
}

bool isFourBitPsm(uint8_t psm) // FEAT-RD3
{
    return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
}

bool isSupportedTex(uint8_t psm, uint8_t cpsm, uint8_t csm) // FEAT-RD3
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return true;
    case GS_PSM_T8:
    case GS_PSM_T8H:
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        if (csm != 0u)
            return false;
        return cpsm == GS_PSM_CT32 || cpsm == GS_PSM_CT24 || cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S; // FEAT-RD3b
    default:
        return false;
    }
}

void buildClutFrom512(uint64_t texa, uint32_t cpsm, uint32_t csa, bool fourBit, const uint16_t *clut512, uint32_t *clut) // FEAT-RD5
{
    const uint32_t count = fourBit ? 16u : 256u;
    const bool sixteen = (cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S);
    const uint32_t csaMask = sixteen ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (csa & csaMask) << 4u;
    const uint32_t ciMask = sixteen ? 0x1FFu : 0xFFu;
    for (uint32_t s = 0; s < count; ++s)
    {
        const uint32_t si = fourBit ? (s & 15u) : s;
        const uint32_t ci = (clutBase + si) & ciMask;
        if (cpsm == GS_PSM_CT32)
        {
            const uint32_t raw = static_cast<uint32_t>(clut512[ci]) | (static_cast<uint32_t>(clut512[ci + 256u]) << 16u);
            clut[s] = applyTexa(texa, static_cast<uint8_t>(cpsm), raw);
        }
        else if (cpsm == GS_PSM_CT24)
        {
            const uint32_t raw = static_cast<uint32_t>(clut512[ci]) | (static_cast<uint32_t>(clut512[ci + 256u]) << 16u);
            clut[s] = applyTexa(texa, static_cast<uint8_t>(cpsm), raw & 0xFFFFFFu);
        }
        else
        {
            clut[s] = applyTexa(texa, static_cast<uint8_t>(cpsm), rgba5551ToRgba8888(clut512[ci]));
        }
    }
}

uint32_t decodeTexel(uint32_t raw, uint8_t psm, uint64_t texa, bool indexed, const uint32_t *clut) // FEAT-RD5
{
    if (indexed)
    {
        const uint32_t idx = isFourBitPsm(psm) ? (raw & 0x0Fu) : (raw & 0xFFu);
        return clut[idx];
    }
    if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
        return applyTexa(texa, psm, rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
    return applyTexa(texa, psm, raw);
}

struct RdFastEntry // FEAT-RD5c
{
    uint64_t tex0Key = 0, texa = 0, contentId = 0;
    uint32_t pageSum = 0, clutGen = 0, first = 0, last = 0, cp = 0;
    bool indexed = false, valid = false;
};
RdFastEntry g_fastCache[1024]; // FEAT-RD5c GS thread only

void writeSerialRing(uint32_t serial, uint64_t contentId) // FEAT-RD5
{
    RdSerialSlot &slot = g_serialRing[serial & (kRdRingSize - 1u)];
    slot.serial.store(0u, std::memory_order_release);
    slot.contentId.store(contentId, std::memory_order_relaxed);
    slot.serial.store(serial, std::memory_order_release);
}
void storeFast(uint32_t idx, uint64_t tex0Key, uint64_t texa, uint64_t cid, uint32_t first, uint32_t last, uint32_t cp, bool indexed) // FEAT-RD5c
{
    RdFastEntry &e = g_fastCache[idx];
    e.tex0Key = tex0Key; e.texa = texa; e.contentId = cid;
    e.first = first; e.last = last; e.cp = cp; e.indexed = indexed; e.clutGen = g_rdClutGen;
    uint32_t sum = 0;
    for (uint32_t p = first; p <= last; ++p)
        sum += g_rdPageGen[p];
    if (indexed)
        sum += g_rdPageGen[cp];
    e.pageSum = sum;
    e.valid = true;
}
} // namespace

void ps2xRdNoteClut(uint32_t tbp0, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cpsm, uint32_t csa, const uint16_t *clut512) // FEAT-RD3c
{
    RdClutNote n;
    std::memcpy(n.clut, clut512, sizeof(n.clut));
    n.cpsm = cpsm;
    n.csa = csa;
    n.clutHash = fnv1a64(n.clut, sizeof(n.clut));
    n.clutHash = fnv1a64(&cpsm, sizeof(cpsm), n.clutHash);
    n.clutHash = fnv1a64(&csa, sizeof(csa), n.clutHash);
    std::lock_guard<std::mutex> lk(g_clutMutex);
    g_clutNotes[RdClutKey{tbp0, tbw, psm, tw, th}] = n;
}

void ps2xRdSetVram(const uint8_t *vram) // FEAT-RD3
{
    g_rdVram.store(vram, std::memory_order_release);
}

void ps2xRdTexTickFrame() // FEAT-RD3
{
    ++g_texFrame;
    ++g_texFramesSinceLog; // FEAT-RD5
    for (auto it = g_texCache.begin(); it != g_texCache.end();)
    {
        if ((g_texFrame - it->second.lastFrame) >= 600u)
        {
            if (it->second.glId != 0u)
                rlUnloadTexture(it->second.glId);
            it = g_texCache.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = g_glByContentId.begin(); it != g_glByContentId.end();) // FEAT-RD5
    {
        if ((g_texFrame - it->second.lastFrame) >= 600u)
        {
            if (it->second.glId != 0u)
                rlUnloadTexture(it->second.glId);
            it = g_glByContentId.erase(it);
        }
        else
        {
            ++it;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastTexLog).count();
    if (elapsedMs >= 5000)
    {
        const double secs = static_cast<double>(elapsedMs) / 1000.0;
        const double fr = g_texFramesSinceLog ? static_cast<double>(g_texFramesSinceLog) : 1.0; // FEAT-RD5
        ps2xSafeLog("[rdtex] live=%u decodes/s=%.1f unsupported=%u notedClut=%u fallbackClut=%u exact=%u fallback=%u noteCalls=%u fullDecodes=%u fastHits=%u rawHits=%u\n",
                    static_cast<unsigned>(g_texCache.size() + g_glByContentId.size()),
                    static_cast<double>(g_decodesSinceLog) / secs,
                    g_unsupportedSinceLog, g_notedClutSinceLog, g_fallbackClutSinceLog,
                    static_cast<unsigned>(static_cast<double>(g_exactSinceLog) / fr),
                    static_cast<unsigned>(static_cast<double>(g_fallbackSinceLog) / fr),
                    static_cast<unsigned>(g_rdNoteCalls.exchange(0u)), static_cast<unsigned>(g_rdFullDecodes.exchange(0u)), static_cast<unsigned>(g_rdFastHits.exchange(0u)), static_cast<unsigned>(g_rdRawHits.exchange(0u))); // FEAT-RD5 FEAT-RD5b
        g_notedClutSinceLog = 0;                                                         // FEAT-RD3c
        g_fallbackClutSinceLog = 0;                                                      // FEAT-RD3c
        g_exactSinceLog = 0;                                                             // FEAT-RD5
        g_fallbackSinceLog = 0;                                                          // FEAT-RD5
        g_texFramesSinceLog = 0;                                                         // FEAT-RD5
        g_decodesSinceLog = 0;
        g_unsupportedSinceLog = 0;
        g_lastTexLog = now;
    }
}

unsigned int ps2xRdGetTexture(uint64_t tex0, uint64_t texa) // FEAT-RD3
{
    const uint8_t *vram = g_rdVram.load(std::memory_order_acquire);
    if (!vram)
        return 0u;

    const uint32_t tbp0 = static_cast<uint32_t>(tex0 & 0x3FFFull);
    const uint32_t tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Full);
    const uint8_t psm = static_cast<uint8_t>((tex0 >> 20) & 0x3Full);
    const uint32_t tw = static_cast<uint32_t>((tex0 >> 26) & 0xFull);
    const uint32_t th = static_cast<uint32_t>((tex0 >> 30) & 0xFull);
    const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFull);
    const uint8_t cpsm = static_cast<uint8_t>((tex0 >> 51) & 0xFull);
    const uint8_t csm = static_cast<uint8_t>((tex0 >> 55) & 0x1ull);
    const uint8_t csa = static_cast<uint8_t>((tex0 >> 56) & 0x1Full);

    if (!isSupportedTex(psm, cpsm, csm))
    {
        ++g_unsupportedSinceLog;
        return 0u;
    }

    static const bool s_checkEveryFrame = []() {
        const char *e = std::getenv("PS2X_RD_TEXCHECK");
        return e && e[0] == '1';
    }();

    // Key masks out TCC (bit 34) and TFX (bits 35-36) which only affect shading.
    const uint64_t tex0Key = tex0 & (0x3FFFFFFFFull | (0xFFFFFFull << 37));
    const RdTexKey key{tex0Key, texa};

    // FEAT-RD3c: noted CLUT (the one the GS had active at draw time)
    RdClutNote note{};
    bool haveNote = false;
    if (isIndexedPsm(psm))
    {
        std::lock_guard<std::mutex> lk(g_clutMutex);
        auto nit = g_clutNotes.find(RdClutKey{tbp0, tbw, psm, tw, th});
        if (nit != g_clutNotes.end())
        {
            note = nit->second;
            haveNote = true;
        }
    }
    const uint64_t curClutHash = haveNote ? note.clutHash : 0ull; // FEAT-RD3c

    auto it = g_texCache.find(key);
    if (it != g_texCache.end() && it->second.clutHash == curClutHash) // FEAT-RD3c
    {
        if (it->second.lastFrame == g_texFrame)
            return it->second.glId;
        if (!s_checkEveryFrame && (g_texFrame - it->second.lastCheckFrame) < 30u)
        {
            it->second.lastFrame = g_texFrame;
            return it->second.glId;
        }
    }

    const uint32_t w = 1u << std::min<uint32_t>(tw, 10u);
    const uint32_t h = 1u << std::min<uint32_t>(th, 10u);

    static std::vector<uint8_t> s_rgba;
    s_rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    auto *dst = reinterpret_cast<uint32_t *>(s_rgba.data());

    GSMem::TexturePageCache pageCache;
    uint32_t clut[256] = {};
    const bool indexed = isIndexedPsm(psm);
    if (indexed)
    {
        const bool fourBit = isFourBitPsm(psm);
        const bool sixteenBit = (cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S); // FEAT-RD3b
        const uint32_t count = fourBit ? 16u : 256u;

        if (haveNote) // FEAT-RD3c
        {
            ++g_notedClutSinceLog;
            const bool nSixteen = (note.cpsm == GS_PSM_CT16 || note.cpsm == GS_PSM_CT16S);
            const uint32_t csaMask = nSixteen ? 0x1Fu : 0x0Fu;
            const uint32_t clutBase = (note.csa & csaMask) << 4u;
            const uint32_t ciMask = nSixteen ? 0x1FFu : 0xFFu;
            for (uint32_t s = 0; s < count; ++s)
            {
                const uint32_t si = fourBit ? (s & 15u) : s;
                const uint32_t ci = (clutBase + si) & ciMask;
                if (note.cpsm == GS_PSM_CT32)
                {
                    const uint32_t raw = static_cast<uint32_t>(note.clut[ci]) | (static_cast<uint32_t>(note.clut[ci + 256u]) << 16u);
                    clut[s] = applyTexa(texa, static_cast<uint8_t>(note.cpsm), raw);
                }
                else if (note.cpsm == GS_PSM_CT24)
                {
                    const uint32_t raw = static_cast<uint32_t>(note.clut[ci]) | (static_cast<uint32_t>(note.clut[ci + 256u]) << 16u);
                    clut[s] = applyTexa(texa, static_cast<uint8_t>(note.cpsm), raw & 0xFFFFFFu);
                }
                else
                {
                    clut[s] = applyTexa(texa, static_cast<uint8_t>(note.cpsm), rgba5551ToRgba8888(note.clut[ci]));
                }
            }
        }
        else
        {
        ++g_fallbackClutSinceLog; // FEAT-RD3c
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t srcIdx = fourBit ? i : swizzleClutIndexCSM1(i); // FEAT-RD3b
            const uint32_t sx = srcIdx & 0x0Fu;
            const uint32_t sy = srcIdx >> 4u;
            const uint32_t raw = GSMem::ReadTexture(pageCache, vram, cpsm, cbp, 1u, sx, sy);
            if (sixteenBit)
                clut[i] = applyTexa(texa, cpsm, rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
            else if (cpsm == GS_PSM_CT24)
                clut[i] = applyTexa(texa, cpsm, raw & 0xFFFFFFu);
            else
                clut[i] = raw;
        }
        } // FEAT-RD3c
    }

    const uint32_t effTbw = std::max<uint32_t>(tbw, 1u);
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            const uint32_t raw = GSMem::ReadTexture(pageCache, vram, psm, tbp0, effTbw, x, y);
            uint32_t rgba = 0u;
            if (indexed)
            {
                const uint32_t idx = isFourBitPsm(psm) ? (raw & 0x0Fu) : (raw & 0xFFu);
                rgba = clut[idx];
            }
            else if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
            {
                rgba = applyTexa(texa, psm, rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
            }
            else
            {
                rgba = applyTexa(texa, psm, raw);
            }
            dst[static_cast<size_t>(y) * w + x] = rgba;
        }
    }

    ++g_decodesSinceLog;

    const uint32_t params[9] = {tbp0, tbw, psm, tw, th, cbp, cpsm, csa, 0u};
    uint64_t hash = fnv1a64(params, sizeof(params) - sizeof(uint32_t));
    hash = fnv1a64(&texa, sizeof(texa), hash);
    hash = fnv1a64(s_rgba.data(), s_rgba.size(), hash);

    { // FEAT-RD3b texture dump
        static const bool s_dump = []() {
            const char *e = std::getenv("PS2X_RD_TEXDUMP");
            return e && e[0] == '1';
        }();
        static std::unordered_set<uint64_t> s_dumped;
        if (s_dump && s_dumped.size() < 200u && s_dumped.insert(hash).second)
        {
            std::error_code ec;
            std::filesystem::create_directories("logs/rdtex", ec);
            std::vector<uint8_t> copy(s_rgba);
            for (size_t i = 3; i < copy.size(); i += 4)
                copy[i] = static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>(copy[i]) * 2u, 255u));
            Image img{copy.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            char path[256];
            std::snprintf(path, sizeof(path), "logs/rdtex/%016llx_psm%u_%ux%u.png",
                          static_cast<unsigned long long>(hash), static_cast<unsigned>(psm), w, h);
            ExportImage(img, path);
            if (FILE *f = std::fopen("logs/rdtex/index.txt", "a"))
            {
                std::fprintf(f, "%016llx tex0=%016llx psm=%u tw=%u th=%u tbp0=%u tbw=%u cbp=%u cpsm=%u csa=%u\n",
                             static_cast<unsigned long long>(hash), static_cast<unsigned long long>(tex0),
                             static_cast<unsigned>(psm), tw, th, tbp0, tbw, cbp, static_cast<unsigned>(cpsm),
                             static_cast<unsigned>(csa));
                std::fclose(f);
            }
        }
    }

    if (it != g_texCache.end())
    {
        it->second.lastFrame = g_texFrame;
        it->second.lastCheckFrame = g_texFrame;
        if (it->second.contentHash != hash)
        {
            if (it->second.glId != 0u)
                rlUnloadTexture(it->second.glId);
            it->second.glId = rlLoadTexture(s_rgba.data(), static_cast<int>(w), static_cast<int>(h),
                                            PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
            it->second.contentHash = hash;
        }
        it->second.clutHash = curClutHash; // FEAT-RD3c
        return it->second.glId;
    }

    const unsigned int glId = rlLoadTexture(s_rgba.data(), static_cast<int>(w), static_cast<int>(h),
                                            PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    g_texCache.emplace(key, RdTexEntry{glId, hash, g_texFrame, g_texFrame, curClutHash}); // FEAT-RD3c
    return glId;
}

void ps2xRdNoteDraw(uint32_t serial, uint64_t tex0, uint64_t texa, const uint8_t *vram, const uint16_t *clut512) // FEAT-RD5
{
    if (serial == 0u)
        return;
    if (!vram)
        vram = g_rdVram.load(std::memory_order_acquire);
    if (!vram)
        return;

    const uint32_t tbp0 = static_cast<uint32_t>(tex0 & 0x3FFFull);
    const uint32_t tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Full);
    const uint8_t psm = static_cast<uint8_t>((tex0 >> 20) & 0x3Full);
    const uint32_t tw = static_cast<uint32_t>((tex0 >> 26) & 0xFull);
    const uint32_t th = static_cast<uint32_t>((tex0 >> 30) & 0xFull);
    const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFull);
    const uint8_t cpsm = static_cast<uint8_t>((tex0 >> 51) & 0xFull);
    const uint8_t csm = static_cast<uint8_t>((tex0 >> 55) & 0x1ull);
    const uint8_t csa = static_cast<uint8_t>((tex0 >> 56) & 0x1Full);

    if (!isSupportedTex(psm, cpsm, csm))
        return;

    const uint64_t tex0Key = tex0 & (0x3FFFFFFFFull | (0xFFFFFFull << 37));
    static thread_local uint32_t t_lastSerial = 0u;
    static thread_local uint64_t t_lastTex0Key = 0ull;
    static thread_local uint64_t t_lastTexa = 0ull;
    static thread_local uint64_t t_lastContentId = 0ull;
    const uint32_t fastIdx = static_cast<uint32_t>(((tex0Key * 0x9E3779B97F4A7C15ull) ^ (texa * 0xC2B2AE3D27D4EB4Full)) >> 54) & 1023u; // FEAT-RD5c
    {
        const RdFastEntry &fe = g_fastCache[fastIdx]; // FEAT-RD5c
        if (fe.valid && fe.tex0Key == tex0Key && fe.texa == texa && (!fe.indexed || fe.clutGen == g_rdClutGen))
        {
            uint32_t sum = 0;
            for (uint32_t p = fe.first; p <= fe.last; ++p)
                sum += g_rdPageGen[p];
            if (fe.indexed)
                sum += g_rdPageGen[fe.cp];
            if (sum == fe.pageSum)
            {
                g_rdNoteCalls.fetch_add(1u, std::memory_order_relaxed);
                g_rdFastHits.fetch_add(1u, std::memory_order_relaxed);
                writeSerialRing(serial, fe.contentId);
                return;
            }
        }
    }
    if (serial == t_lastSerial && tex0Key == t_lastTex0Key && texa == t_lastTexa && t_lastContentId != 0ull)
    {
        writeSerialRing(serial, t_lastContentId);
        return;
    }

    const uint32_t w = 1u << std::min<uint32_t>(tw, 10u);
    const uint32_t h = 1u << std::min<uint32_t>(th, 10u);
    g_rdNoteCalls.fetch_add(1u, std::memory_order_relaxed); // FEAT-RD5b

    // FEAT-RD5b: cheap key from VRAM page write generations + CLUT bytes; full decode only when unseen.
    static std::unordered_map<uint64_t, uint64_t> s_keyToContent; // GS thread only
    uint64_t quickKey = 0ull;
    uint32_t fpFirst = 0u, fpLast = 0u, fpCp = 0u; // FEAT-RD5c
    {
        uint32_t pageH = 32u, bppNum = 4u, bppDen = 1u;
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: pageH = 64u; bppNum = 2u; break;
        case GS_PSM_T8: pageH = 64u; bppNum = 1u; break;
        case GS_PSM_T4: pageH = 128u; bppNum = 1u; bppDen = 2u; break;
        default: break;
        }
        const uint32_t rows = ((h + pageH - 1u) / pageH) * pageH;
        const uint32_t widthPx = std::max<uint32_t>(std::max<uint32_t>(tbw, 1u) * 64u, w);
        const uint64_t bytes = (static_cast<uint64_t>(widthPx) * rows * bppNum + bppDen - 1u) / bppDen;
        const uint32_t first = (tbp0 * 256u) / 8192u;
        const uint32_t last = std::min<uint32_t>(static_cast<uint32_t>((static_cast<uint64_t>(tbp0) * 256u + bytes - 1u) / 8192u), 511u);
        fpFirst = std::min<uint32_t>(first, 511u);
        fpLast = std::min<uint32_t>(last, 511u);
        fpCp = std::min<uint32_t>((cbp * 256u) / 8192u, 511u);
        uint64_t gh = 0xcbf29ce484222325ull;
        for (uint32_t p = first; p <= last && p < 512u; ++p)
            gh = (gh ^ g_rdPageGen[p]) * 0x100000001b3ull;
        if (isIndexedPsm(psm))
        {
            const uint32_t cp = std::min<uint32_t>((cbp * 256u) / 8192u, 511u);
            gh = (gh ^ g_rdPageGen[cp]) * 0x100000001b3ull;
            if (clut512)
                gh = fnv1a64(clut512, 512u * sizeof(uint16_t), gh);
        }
        quickKey = fnv1a64(&tex0Key, sizeof(tex0Key), gh);
        quickKey = fnv1a64(&texa, sizeof(texa), quickKey);
        auto itKey = s_keyToContent.find(quickKey);
        if (itKey != s_keyToContent.end())
        {
            t_lastSerial = serial;
            t_lastTex0Key = tex0Key;
            t_lastTexa = texa;
            t_lastContentId = itKey->second;
            storeFast(fastIdx, tex0Key, texa, itKey->second, fpFirst, fpLast, fpCp, isIndexedPsm(psm)); // FEAT-RD5c
            writeSerialRing(serial, itKey->second);
            return;
        }
    }
    // FEAT-RD5d: raw VRAM CRC key (no per-texel reads); falls through to full decode only if unseen.
    static std::unordered_map<uint64_t, uint64_t> s_rawToContent; // GS thread only
    uint64_t rawKey = 0ull;
    {
        const uint64_t vramSize = 4ull * 1024ull * 1024ull;
        const uint64_t b0 = static_cast<uint64_t>(fpFirst) * 8192ull;
        uint64_t b1 = (static_cast<uint64_t>(fpLast) + 1ull) * 8192ull;
        if (b1 > vramSize)
            b1 = vramSize;
        uint64_t c0 = 0xFFFFFFFFull, c1 = 0x12345678ull;
        const uint8_t *p = vram + b0;
        uint64_t n = (b1 > b0) ? (b1 - b0) : 0ull;
        for (; n >= 16ull; n -= 16ull, p += 16)
        {
            uint64_t a, b;
            std::memcpy(&a, p, 8);
            std::memcpy(&b, p + 8, 8);
            c0 = _mm_crc32_u64(c0, a);
            c1 = _mm_crc32_u64(c1, b);
        }
        for (; n >= 8ull; n -= 8ull, p += 8)
        {
            uint64_t a;
            std::memcpy(&a, p, 8);
            c0 = _mm_crc32_u64(c0, a);
        }
        uint64_t k = (c0 << 32) | (c1 & 0xFFFFFFFFull);
        k = fnv1a64(&k, sizeof(k), 0xcbf29ce484222325ull);
        const uint32_t fields2[4] = {static_cast<uint32_t>(b1 - b0), tbw, cpsm, csa};
        k = fnv1a64(&tex0Key, sizeof(tex0Key), k);
        k = fnv1a64(&texa, sizeof(texa), k);
        k = fnv1a64(fields2, sizeof(fields2), k);
        if (isIndexedPsm(psm))
        {
            if (clut512)
            {
                uint32_t cc = 0xFFFFFFFFu;
                const uint8_t *cp8 = reinterpret_cast<const uint8_t *>(clut512);
                for (uint32_t i = 0; i < 1024u; i += 8)
                {
                    uint64_t a;
                    std::memcpy(&a, cp8 + i, 8);
                    cc = static_cast<uint32_t>(_mm_crc32_u64(cc, a));
                }
                k = fnv1a64(&cc, sizeof(cc), k);
            }
            else
            {
                const uint64_t cb0 = static_cast<uint64_t>(fpCp) * 8192ull;
                if (cb0 + 8192ull <= vramSize)
                    k = fnv1a64(vram + cb0, 8192u, k);
            }
        }
        rawKey = k;
        auto itRaw = s_rawToContent.find(rawKey);
        if (itRaw != s_rawToContent.end())
        {
            g_rdRawHits.fetch_add(1u, std::memory_order_relaxed);
            if (s_keyToContent.size() > 200000u)
                s_keyToContent.clear();
            s_keyToContent[quickKey] = itRaw->second;
            t_lastSerial = serial;
            t_lastTex0Key = tex0Key;
            t_lastTexa = texa;
            t_lastContentId = itRaw->second;
            storeFast(fastIdx, tex0Key, texa, itRaw->second, fpFirst, fpLast, fpCp, isIndexedPsm(psm));
            writeSerialRing(serial, itRaw->second);
            return;
        }
    }
    g_rdFullDecodes.fetch_add(1u, std::memory_order_relaxed);

    GSMem::TexturePageCache pageCache;
    uint32_t clut[256] = {};
    uint64_t clutHash = 0ull;
    const bool indexed = isIndexedPsm(psm);
    if (indexed)
    {
        const bool fourBit = isFourBitPsm(psm);
        const uint32_t count = fourBit ? 16u : 256u;
        if (clut512)
        {
            buildClutFrom512(texa, cpsm, csa, fourBit, clut512, clut);
        }
        else
        {
            const bool sixteenBit = (cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t srcIdx = fourBit ? i : swizzleClutIndexCSM1(i);
                const uint32_t raw = GSMem::ReadTexture(pageCache, vram, cpsm, cbp, 1u, srcIdx & 0x0Fu, srcIdx >> 4u);
                if (sixteenBit)
                    clut[i] = applyTexa(texa, cpsm, rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
                else if (cpsm == GS_PSM_CT24)
                    clut[i] = applyTexa(texa, cpsm, raw & 0xFFFFFFu);
                else
                    clut[i] = raw;
            }
        }
        clutHash = fnv1a64(clut, static_cast<size_t>(count) * sizeof(uint32_t));
    }

    static thread_local std::vector<uint32_t> t_raw;
    t_raw.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    const uint32_t effTbw = std::max<uint32_t>(tbw, 1u);
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
            t_raw[static_cast<size_t>(y) * w + x] = GSMem::ReadTexture(pageCache, vram, psm, tbp0, effTbw, x, y);
    }

    const uint32_t fields[7] = {tbp0, tbw, psm, tw, th, cpsm, csa};
    uint64_t contentId = fnv1a64(fields, sizeof(fields));
    contentId = fnv1a64(&texa, sizeof(texa), contentId);
    contentId = fnv1a64(&clutHash, sizeof(clutHash), contentId);
    contentId = fnv1a64(t_raw.data(), t_raw.size() * sizeof(uint32_t), contentId);
    if (contentId == 0ull)
        contentId = 1ull;

    {
        std::lock_guard<std::mutex> lk(g_contentMutex);
        if (g_contentMap.find(contentId) == g_contentMap.end())
        {
            RdContentEntry entry;
            entry.w = w;
            entry.h = h;
            entry.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            auto *dst = reinterpret_cast<uint32_t *>(entry.rgba.data());
            for (size_t i = 0; i < t_raw.size(); ++i)
                dst[i] = decodeTexel(t_raw[i], psm, texa, indexed, clut);
            g_contentMap.emplace(contentId, std::move(entry));
        }
    }

    if (s_keyToContent.size() > 200000u)
        s_keyToContent.clear();
    s_keyToContent[quickKey] = contentId; // FEAT-RD5b
    if (s_rawToContent.size() > 100000u)
        s_rawToContent.clear();
    s_rawToContent[rawKey] = contentId; // FEAT-RD5d
    t_lastSerial = serial;
    t_lastTex0Key = tex0Key;
    t_lastTexa = texa;
    t_lastContentId = contentId;
    storeFast(fastIdx, tex0Key, texa, contentId, fpFirst, fpLast, fpCp, indexed); // FEAT-RD5c
    writeSerialRing(serial, contentId);
}

uint64_t ps2xRdLookupContentId(uint32_t serial) // FEAT-RD5
{
    if (serial == 0u)
        return 0ull;
    const RdSerialSlot &slot = g_serialRing[serial & (kRdRingSize - 1u)];
    const uint32_t s1 = slot.serial.load(std::memory_order_acquire);
    if (s1 != serial)
        return 0ull;
    const uint64_t cid = slot.contentId.load(std::memory_order_relaxed);
    if (slot.serial.load(std::memory_order_acquire) != serial)
        return 0ull;
    return cid;
}

unsigned int ps2xRdGetTextureForBatch(uint32_t serial, uint64_t tex0, uint64_t texa) // FEAT-RD5
{
    const uint64_t cid = ps2xRdLookupContentId(serial);
    if (cid != 0ull)
    {
        auto it = g_glByContentId.find(cid);
        if (it != g_glByContentId.end())
        {
            it->second.lastFrame = g_texFrame;
            ++g_exactSinceLog;
            return it->second.glId;
        }
        uint32_t w = 0u, h = 0u;
        std::vector<uint8_t> rgbaCopy;
        {
            std::lock_guard<std::mutex> lk(g_contentMutex);
            auto cit = g_contentMap.find(cid);
            if (cit != g_contentMap.end())
            {
                w = cit->second.w;
                h = cit->second.h;
                rgbaCopy = cit->second.rgba;
            }
        }
        if (w > 0u && h > 0u && !rgbaCopy.empty())
        {
            const unsigned int glId = rlLoadTexture(rgbaCopy.data(), static_cast<int>(w), static_cast<int>(h),
                                                    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
            g_glByContentId.emplace(cid, RdContentGl{glId, g_texFrame});
            ++g_decodesSinceLog;
            ++g_exactSinceLog;
            return glId;
        }
    }
    ++g_fallbackSinceLog;
    return ps2xRdGetTexture(tex0, texa);
}
