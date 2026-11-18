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

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD3

namespace
{
std::atomic<const uint8_t *> g_rdVram{nullptr}; // FEAT-RD3

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

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastTexLog).count();
    if (elapsedMs >= 5000)
    {
        const double secs = static_cast<double>(elapsedMs) / 1000.0;
        ps2xSafeLog("[rdtex] live=%u decodes/s=%.1f unsupported=%u notedClut=%u fallbackClut=%u\n",
                    static_cast<unsigned>(g_texCache.size()),
                    static_cast<double>(g_decodesSinceLog) / secs,
                    g_unsupportedSinceLog, g_notedClutSinceLog, g_fallbackClutSinceLog); // FEAT-RD3c
        g_notedClutSinceLog = 0;                                                         // FEAT-RD3c
        g_fallbackClutSinceLog = 0;                                                      // FEAT-RD3c
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
