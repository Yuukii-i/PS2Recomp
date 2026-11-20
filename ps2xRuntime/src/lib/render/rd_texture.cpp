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
#include <string> // FEAT-RD6
#include <cctype> // FEAT-RD6
#include <mutex> // FEAT-RD3c
#include <cstring> // FEAT-RD3c
#include <unordered_map>
#include <vector>
#include <nmmintrin.h> // FEAT-RD5d
#include <intrin.h>    // FEAT-RD45

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD3
extern bool ps2xRdActive(); // FEAT-RD49
extern bool ps2xVkSyncPagesForCpu(uint32_t firstPage, uint32_t lastPage, uint32_t *outBytes = nullptr); // FEAT-RD28
extern bool g_rdpOn; // FEAT-RD45
uint64_t g_rdpNoteSubCyc[6] = {0, 0, 0, 0, 0, 0}; // FEAT-RD45: 0=rt, 1=fast, 2=clut, 3=quickKey, 4=rawCrc, 5=decode
uint64_t g_rdpRtSubCyc[4] = {0, 0, 0, 0}; // FEAT-RD46: 0=rt_a(fb), 1=rt_b(mask), 2=rt_c(mutex), 3=rt_d(sync)
std::atomic<uint32_t> g_rdKickSerial{0};     // FEAT-RD5
uint32_t g_rdPageGen[512];                   // FEAT-RD5b VRAM 8 KB page write generations (GS thread)
uint32_t g_rdVramGen = 1;                    // FEAT-RD45 coarse global VRAM write generation (GS thread)
std::atomic<uint32_t> g_rdNoteCalls{0};      // FEAT-RD5b
std::atomic<uint32_t> g_rdFullDecodes{0};    // FEAT-RD5b
uint32_t g_rdClutGen = 0;                    // FEAT-RD5c bumped on every CLUT load (GS thread)
std::atomic<uint32_t> g_rdRawHits{0};        // FEAT-RD5d
std::atomic<uint32_t> g_rdFastHits{0};     // FEAT-RD5c
std::atomic<uint32_t> g_rdStateChangesInKick{0}; // FEAT-RD17
std::atomic<uint32_t> g_rdMergedSpan{0};         // FEAT-RD17
std::atomic<uint32_t> g_rdRtSyncs{0};            // FEAT-RD28
std::atomic<uint32_t> g_rdRtAsyncSkips{0};       // FEAT-RD47
std::atomic<uint32_t> g_rdRtForced{0};           // FEAT-RD47
extern uint32_t ps2xRdFbPageRanges(uint32_t *first, uint32_t *last); // FEAT-RD28b
std::atomic<uint64_t> g_rdRtBytes{0};            // FEAT-RD28
void ps2xRdMarkVramWrite(uint32_t firstByte, uint32_t byteCount) // FEAT-RD5b
{
    if (byteCount == 0u || firstByte >= 0x400000u)
        return;
    ++g_rdVramGen; // FEAT-RD45
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
RdSerialSlot g_serialRing3d[kRdRingSize]; // FEAT-RD5 / FIX-RD72
RdSerialSlot g_serialRing2d[kRdRingSize]; // FIX-RD72
std::atomic<uint32_t> g_fbFeedbackRing3d[kRdRingSize]; // FEAT-RD28b / FIX-RD72
std::atomic<uint32_t> g_fbFeedbackRing2d[kRdRingSize]; // FIX-RD72

static const bool g_splitRing = []() { // FIX-RD72
    const char *e = std::getenv("PS2X_RD_SPLITRING");
    return !(e && e[0] == '0');
}();

template <typename T>
inline T &rdRingSlot(T *ring3d, T *ring2d, uint32_t serial) // FIX-RD72
{
    T *ring = (g_splitRing && (serial & 0x80000000u)) ? ring2d : ring3d;
    return ring[serial & (kRdRingSize - 1u)];
}

struct RdStateSlot // FEAT-RD17
{
    std::atomic<uint32_t> serial{0};
    std::atomic<uint64_t> prim{0};
    std::atomic<uint64_t> alpha{0};
    std::atomic<uint64_t> test{0};
    std::atomic<uint64_t> frame{0};
    std::atomic<uint64_t> zbuf{0};
    std::atomic<uint64_t> tex0{0};
    std::atomic<uint64_t> tex1{0};
    std::atomic<uint64_t> clamp{0};
    std::atomic<uint64_t> texa{0};
    std::atomic<uint64_t> fba{0};
    std::atomic<uint64_t> xyoff{0}; // FEAT-RD21: ofx | ofy<<32 (12.4)
    std::atomic<uint64_t> scis{0};  // FEAT-RD22: SCISSOR packed x0 | x1<<16 | y0<<32 | y1<<48
};
RdStateSlot g_stateRing[kRdRingSize]; // FEAT-RD17

struct RdContentEntry // FEAT-RD5
{
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t psm = 0; // FEAT-RD6
    std::vector<uint8_t> rgba;
};
std::mutex g_contentMutex;                                // FEAT-RD5
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

// FEAT-RD6: texture dump (RD5 path) + replacement (main thread only)
uint32_t g_replacedSinceLog = 0;
std::unordered_map<std::string, std::string> g_replaceMap;
bool g_replaceScanned = false;
std::unordered_set<uint64_t> g_replaceFailed;
uint32_t g_rd6Dumped = 0;

void rd6Scan()
{
    g_replaceScanned = true;
    const char *e = std::getenv("PS2X_TEX_REPLACE_DIR");
    const std::filesystem::path dir = (e && e[0]) ? e : "mods/textures";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return;
    for (const auto &de : std::filesystem::directory_iterator(dir, ec))
    {
        if (!de.is_regular_file(ec))
            continue;
        std::string ext = de.path().extension().string();
        for (auto &c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png")
            continue;
        std::string name = de.path().filename().string();
        if (name.size() < 16u)
            continue;
        std::string key = name.substr(0, 16);
        bool ok = true;
        for (auto &c : key)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                ok = false;
        }
        if (ok)
            g_replaceMap[key] = de.path().string();
    }
    ps2xSafeLog("[texrep] scanned %u replacement files\n", static_cast<unsigned>(g_replaceMap.size()));
}

unsigned int rd6Upload(uint64_t cid, uint32_t w, uint32_t h, uint32_t psm, const std::vector<uint8_t> &rgba)
{
    static const bool s_dump = []() {
        const char *e = std::getenv("PS2X_RD_TEXDUMP");
        return e && e[0] == '1';
    }();
    char idStr[24];
    std::snprintf(idStr, sizeof(idStr), "%016llx", static_cast<unsigned long long>(cid));
    if (s_dump && g_rd6Dumped < 2000u)
    {
        ++g_rd6Dumped;
        std::error_code ec;
        std::filesystem::create_directories("logs/rdtex", ec);
        std::vector<uint8_t> copy(rgba);
        for (size_t i = 3; i < copy.size(); i += 4)
            copy[i] = static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>(copy[i]) * 2u, 255u));
        Image img{copy.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        char path[256];
        std::snprintf(path, sizeof(path), "logs/rdtex/%s_psm%u_%ux%u.png", idStr, static_cast<unsigned>(psm), w, h);
        ExportImage(img, path);
    }
    if (!g_replaceScanned)
        rd6Scan();
    auto rit = g_replaceMap.find(idStr);
    if (rit != g_replaceMap.end() && g_replaceFailed.find(cid) == g_replaceFailed.end())
    {
        Image img = LoadImage(rit->second.c_str());
        if (img.data != nullptr && img.width > 0 && img.height > 0)
        {
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            auto *p = static_cast<uint8_t *>(img.data);
            const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
            for (size_t i = 0; i < n; ++i)
                p[i * 4 + 3] = static_cast<uint8_t>((static_cast<uint32_t>(p[i * 4 + 3]) + 1u) / 2u);
            const unsigned int glId = rlLoadTexture(img.data, img.width, img.height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
            int mips = 1;
            rlGenTextureMipmaps(glId, img.width, img.height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &mips);
            rlTextureParameters(glId, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_MIP_LINEAR);
            ps2xSafeLog("[texrep] %s <- %s (%dx%d)\n", idStr, rit->second.c_str(), img.width, img.height);
            UnloadImage(img);
            ++g_replacedSinceLog;
            return glId;
        }
        if (img.data != nullptr)
            UnloadImage(img);
        g_replaceFailed.insert(cid);
        ps2xSafeLog("[texrep] %s: failed to load %s, using decoded texture\n", idStr, rit->second.c_str());
    }
    return rlLoadTexture(rgba.data(), static_cast<int>(w), static_cast<int>(h), PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
}

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

constexpr uint32_t kFastSets = 2048u; // FEAT-RD45: 2-way set-associative (4096 entries)
struct RdFastEntry // FEAT-RD5c / FEAT-RD45
{
    uint64_t tex0Key = 0, texa = 0, contentId = 0, clutHash = 0;
    uint32_t vramGen = 0, pageSum = 0, clutGen = 0, first = 0, last = 0, cp = 0;
    bool indexed = false, valid = false;
};
RdFastEntry g_fastCache[kFastSets][2]; // FEAT-RD45 GS thread only
uint8_t g_fastRepl[kFastSets] = {};    // FEAT-RD45

inline uint64_t getClutHashFast(const uint16_t *clut512) // FEAT-RD45: O(1) cached CRC32 of clut512 per g_rdClutGen
{
    if (!clut512)
        return 0ull;
    static const uint16_t *s_lastPtr = nullptr;
    static uint32_t s_lastGen = 0xFFFFFFFFu;
    static uint64_t s_lastHash = 0ull;
    if (clut512 == s_lastPtr && g_rdClutGen == s_lastGen)
        return s_lastHash;
    uint64_t c0 = 0xFFFFFFFFull, c1 = 0x12345678ull;
    const uint8_t *cp8 = reinterpret_cast<const uint8_t *>(clut512);
    for (uint32_t i = 0; i < 1024u; i += 16u)
    {
        uint64_t a, b;
        std::memcpy(&a, cp8 + i, 8);
        std::memcpy(&b, cp8 + i + 8, 8);
        c0 = _mm_crc32_u64(c0, a);
        c1 = _mm_crc32_u64(c1, b);
    }
    s_lastPtr = clut512;
    s_lastGen = g_rdClutGen;
    s_lastHash = (c0 << 32) | (c1 & 0xFFFFFFFFull);
    return s_lastHash;
}

void writeSerialRing(uint32_t serial, uint64_t contentId) // FEAT-RD5
{
    RdSerialSlot &slot = rdRingSlot(g_serialRing3d, g_serialRing2d, serial); // FIX-RD72
    slot.serial.store(0u, std::memory_order_release);
    slot.contentId.store(contentId, std::memory_order_relaxed);
    slot.serial.store(serial, std::memory_order_release);
}
void storeFast(uint32_t setIdx, uint64_t tex0Key, uint64_t texa, uint64_t cid, uint32_t first, uint32_t last, uint32_t cp, bool indexed, uint64_t clutHash) // FEAT-RD5c / FEAT-RD45
{
    uint32_t way = 0u;
    if (g_fastCache[setIdx][0].valid && g_fastCache[setIdx][0].tex0Key == tex0Key && g_fastCache[setIdx][0].texa == texa)
        way = 0u;
    else if (g_fastCache[setIdx][1].valid && g_fastCache[setIdx][1].tex0Key == tex0Key && g_fastCache[setIdx][1].texa == texa)
        way = 1u;
    else if (!g_fastCache[setIdx][0].valid)
        way = 0u;
    else if (!g_fastCache[setIdx][1].valid)
        way = 1u;
    else
    {
        way = g_fastRepl[setIdx] & 1u;
        g_fastRepl[setIdx] ^= 1u;
    }
    RdFastEntry &e = g_fastCache[setIdx][way];
    e.tex0Key = tex0Key; e.texa = texa; e.contentId = cid; e.clutHash = clutHash;
    e.first = first; e.last = last; e.cp = cp; e.indexed = indexed; e.clutGen = g_rdClutGen; e.vramGen = g_rdVramGen;
    uint32_t sum = 0;
    for (uint32_t p = first; p <= last; ++p)
        sum += g_rdPageGen[p];
    if (indexed)
        sum += g_rdPageGen[cp];
    e.pageSum = sum;
    e.valid = true;
}

// PROBE-BT1: black texture diagnostic probe (PS2X_RD_BLACKTEX=1)
static const bool g_probeBt1 = []() {
    const char *e = std::getenv("PS2X_RD_BLACKTEX");
    return e && e[0] == '1';
}();

struct Bt1Info // PROBE-BT1
{
    uint32_t w = 0u;
    uint32_t h = 0u;
    uint64_t clutHash = 0ull;
    bool isBlack = false;
    bool clutNoted = false;
    bool clutAllZero = false;
};

std::mutex g_bt1Mutex;                                      // PROBE-BT1
std::unordered_map<uint64_t, bool> g_bt1LastWasBlack;       // PROBE-BT1
std::unordered_map<uint64_t, Bt1Info> g_bt1CidInfo;         // PROBE-BT1
std::unordered_map<unsigned int, Bt1Info> g_bt1GlInfo;      // PROBE-BT1
std::atomic<uint32_t> g_bt1Lines{0u};                       // PROBE-BT1
std::atomic<uint32_t> g_bt1CntNote{0u};                     // PROBE-BT1
std::atomic<uint32_t> g_bt1CntFallback{0u};                 // PROBE-BT1
std::atomic<uint32_t> g_bt1CntReuse{0u};                    // PROBE-BT1
std::atomic<uint32_t> g_bt1CntChanged{0u};                  // PROBE-BT1
std::atomic<uint32_t> g_bt2Lines{0u};                       // PROBE-BT2
std::atomic<uint32_t> g_bt2Miss[3][5]{};                    // PROBE-BT2: [0=3D, 1=2D, 2=unk][A..E]
static thread_local int g_bt2Kind = -1;                     // PROBE-BT2: 0=3D, 1=2D, -1=unknown

void computeTexPageRange(uint8_t psm, uint32_t w, uint32_t h, uint32_t tbw, uint32_t tbp0, uint32_t cbp,
                         uint32_t &fpFirst, uint32_t &fpLast, uint32_t &fpCp) // PROBE-BT1
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
    fpFirst = std::min<uint32_t>((tbp0 * 256u) / 8192u, 511u);
    fpLast = std::min<uint32_t>(static_cast<uint32_t>((static_cast<uint64_t>(tbp0) * 256u + bytes - 1u) / 8192u), 511u);
    if (fpLast < fpFirst)
        fpLast = fpFirst;
    fpCp = std::min<uint32_t>((cbp * 256u) / 8192u, 511u);
}

uint32_t bt1PageGenSum(uint8_t psm, uint32_t w, uint32_t h, uint32_t tbw, uint32_t tbp0, uint32_t cbp, bool indexed) // PROBE-BT1
{
    uint32_t fpFirst = 0u, fpLast = 0u, fpCp = 0u;
    computeTexPageRange(psm, w, h, tbw, tbp0, cbp, fpFirst, fpLast, fpCp);
    uint32_t sum = 0u;
    for (uint32_t p = fpFirst; p <= fpLast; ++p)
        sum += g_rdPageGen[p];
    if (indexed)
        sum += g_rdPageGen[fpCp];
    return sum;
}

bool bt1IsRgbaBlack(const uint8_t *rgba, uint32_t w, uint32_t h) // PROBE-BT1
{
    if (w < 16u || h < 16u)
        return false;
    const auto *px = reinterpret_cast<const uint32_t *>(rgba);
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    size_t sampled = 0u, nonBlack = 0u;
    for (size_t i = 0u; i < total; i += 4u)
    {
        ++sampled;
        if ((px[i] & 0x00FFFFFFu) != 0u)
            ++nonBlack;
    }
    return sampled > 0u && (nonBlack * 50u) < sampled;
}

bool bt1ClutAllZero(bool indexed, const uint32_t *clut) // PROBE-BT1
{
    if (!indexed)
        return false;
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        if (clut[i] != 0u)
            return false;
    }
    return true;
}

void bt1LogLine(const char *pathName, uint64_t tex0, uint8_t psm, uint32_t w, uint32_t h,
                uint32_t tbp0, uint32_t cbp, uint8_t cpsm, bool indexed, bool clutNoted,
                uint64_t clutHash, bool clutAllZero, uint32_t serial, uint32_t pageGenSum) // PROBE-BT1
{
    if (g_bt1Lines.load(std::memory_order_relaxed) < 60u &&
        g_bt1Lines.fetch_add(1u, std::memory_order_relaxed) < 60u)
    {
        ps2xSafeLog("[rdbt] path=%s tex0=%llx psm=%u %ux%u tbp0=%u cbp=%u cpsm=%u indexed=%d clutNoted=%d clutHash=%llx clutAllZero=%d serial=%u g_rdPageGen_sum=%u vramGen=%u\n",
                    pathName, static_cast<unsigned long long>(tex0), static_cast<unsigned>(psm),
                    static_cast<unsigned>(w), static_cast<unsigned>(h),
                    static_cast<unsigned>(tbp0), static_cast<unsigned>(cbp), static_cast<unsigned>(cpsm),
                    indexed ? 1 : 0, clutNoted ? 1 : 0, static_cast<unsigned long long>(clutHash),
                    clutAllZero ? 1 : 0, static_cast<unsigned>(serial),
                    static_cast<unsigned>(pageGenSum), static_cast<unsigned>(g_rdVramGen));
    }
}

void bt1Report(const char *pathName, std::atomic<uint32_t> &counter, bool checkChanged,
               uint64_t tex0, uint64_t tex0Key, uint8_t psm, uint32_t w, uint32_t h,
               uint32_t tbp0, uint32_t cbp, uint8_t cpsm, bool indexed, bool clutNoted,
               uint64_t clutHash, bool clutAllZero, uint32_t serial, uint32_t pageGenSum, bool isBlack) // PROBE-BT1
{
    if (!isBlack)
    {
        if (checkChanged)
        {
            std::lock_guard<std::mutex> lk(g_bt1Mutex);
            if (g_bt1LastWasBlack.size() > 16384u)
                g_bt1LastWasBlack.clear();
            g_bt1LastWasBlack[tex0Key] = false;
        }
        return;
    }

    counter.fetch_add(1u, std::memory_order_relaxed);
    bt1LogLine(pathName, tex0, psm, w, h, tbp0, cbp, cpsm, indexed, clutNoted, clutHash, clutAllZero, serial, pageGenSum);

    if (checkChanged)
    {
        bool becameBlack = false;
        {
            std::lock_guard<std::mutex> lk(g_bt1Mutex);
            auto it = g_bt1LastWasBlack.find(tex0Key);
            if (it != g_bt1LastWasBlack.end() && !it->second)
                becameBlack = true;
            if (g_bt1LastWasBlack.size() > 16384u)
                g_bt1LastWasBlack.clear();
            g_bt1LastWasBlack[tex0Key] = true;
        }
        if (becameBlack)
        {
            g_bt1CntChanged.fetch_add(1u, std::memory_order_relaxed);
            bt1LogLine("changed", tex0, psm, w, h, tbp0, cbp, cpsm, indexed, clutNoted, clutHash, clutAllZero, serial, pageGenSum);
        }
    }
}

void bt1ReportReuseCid(uint64_t cid, uint64_t tex0, uint64_t tex0Key, uint8_t psm,
                       uint32_t tbp0, uint32_t tbw, uint32_t cbp, uint8_t cpsm,
                       bool indexed, uint32_t serial) // PROBE-BT1
{
    Bt1Info info;
    {
        std::lock_guard<std::mutex> lk(g_bt1Mutex);
        auto it = g_bt1CidInfo.find(cid);
        if (it == g_bt1CidInfo.end())
            return;
        info = it->second;
    }
    if (!info.isBlack || info.w < 16u || info.h < 16u)
        return;
    const uint32_t pageSum = bt1PageGenSum(psm, info.w, info.h, tbw, tbp0, cbp, indexed);
    bt1Report("reuse", g_bt1CntReuse, false, tex0, tex0Key, psm, info.w, info.h, tbp0, cbp, cpsm,
              indexed, info.clutNoted, info.clutHash, info.clutAllZero, serial, pageSum, true);
}
} // namespace

void ps2xRdBt2SetKind(int kind) // PROBE-BT2
{
    if (!g_probeBt1)
        return;
    g_bt2Kind = kind;
}

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
    if ((g_texFrame & 15u) == 0u) // PERF-EX5: amortise full-cache eviction scan (600-frame threshold unchanged)
    {
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
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastTexLog).count();
    if (elapsedMs >= 5000)
    {
        const double secs = static_cast<double>(elapsedMs) / 1000.0;
        const double fr = g_texFramesSinceLog ? static_cast<double>(g_texFramesSinceLog) : 1.0; // FEAT-RD5
        const uint32_t rtSyncs = g_rdRtSyncs.exchange(0u, std::memory_order_relaxed); // FEAT-RD28
        const uint64_t rtBytes = g_rdRtBytes.exchange(0ull, std::memory_order_relaxed); // FEAT-RD28
        const uint32_t rtAsyncSkips = g_rdRtAsyncSkips.exchange(0u, std::memory_order_relaxed); // FEAT-RD47
        const uint32_t rtForced = g_rdRtForced.exchange(0u, std::memory_order_relaxed); // FEAT-RD47
        ps2xSafeLog("[rdtex] live=%u decodes/s=%.1f unsupported=%u notedClut=%u fallbackClut=%u exact=%u fallback=%u noteCalls=%u fullDecodes=%u fastHits=%u rawHits=%u replaced=%u rtSyncs=%u rtKB=%u rtAsyncSkips=%u rtForced=%u\n",
                    static_cast<unsigned>(g_texCache.size() + g_glByContentId.size()),
                    static_cast<double>(g_decodesSinceLog) / secs,
                    g_unsupportedSinceLog, g_notedClutSinceLog, g_fallbackClutSinceLog,
                    static_cast<unsigned>(static_cast<double>(g_exactSinceLog) / fr),
                    static_cast<unsigned>(static_cast<double>(g_fallbackSinceLog) / fr),
                    static_cast<unsigned>(g_rdNoteCalls.exchange(0u)), static_cast<unsigned>(g_rdFullDecodes.exchange(0u)), static_cast<unsigned>(g_rdFastHits.exchange(0u)), static_cast<unsigned>(g_rdRawHits.exchange(0u)), static_cast<unsigned>(g_replacedSinceLog),
                    static_cast<unsigned>(static_cast<double>(rtSyncs) / fr),
                    static_cast<unsigned>(static_cast<double>(rtBytes) / (1024.0 * fr)),
                    static_cast<unsigned>(rtAsyncSkips),
                    static_cast<unsigned>(rtForced)); // FEAT-RD5 FEAT-RD5b FEAT-RD6 FEAT-RD28 FEAT-RD47
        g_replacedSinceLog = 0; // FEAT-RD6
        g_notedClutSinceLog = 0;                                                         // FEAT-RD3c
        g_fallbackClutSinceLog = 0;                                                    // FEAT-RD3c
        g_exactSinceLog = 0;                                                             // FEAT-RD5
        g_fallbackSinceLog = 0;                                                          // FEAT-RD5
        g_texFramesSinceLog = 0;                                                         // FEAT-RD5
        g_decodesSinceLog = 0;
        g_unsupportedSinceLog = 0;
        g_lastTexLog = now;
        if (g_probeBt1) // PROBE-BT1
        {
            ps2xSafeLog("[rdbt] 5s counts: note=%u fallback=%u reuse=%u changed=%u\n",
                        static_cast<unsigned>(g_bt1CntNote.exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt1CntFallback.exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt1CntReuse.exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt1CntChanged.exchange(0u, std::memory_order_relaxed)));
            ps2xSafeLog("[rdbt2] 5s miss kind=0(3D) A=%u B=%u C=%u D=%u E=%u | kind=1(2D) A=%u B=%u C=%u D=%u E=%u | kind=-1 A=%u B=%u C=%u D=%u E=%u\n", // PROBE-BT2
                        static_cast<unsigned>(g_bt2Miss[0][0].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[0][1].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[0][2].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[0][3].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[0][4].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[1][0].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[1][1].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[1][2].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[1][3].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[1][4].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[2][0].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[2][1].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[2][2].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[2][3].exchange(0u, std::memory_order_relaxed)),
                        static_cast<unsigned>(g_bt2Miss[2][4].exchange(0u, std::memory_order_relaxed)));
        }
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
    // PERF-EX5: only read clutHash on the hot lookup path; defer 1 KB RdClutNote copy to cache misses.
    uint64_t curClutHash = 0ull; // FEAT-RD3c
    if (isIndexedPsm(psm))
    {
        std::lock_guard<std::mutex> lk(g_clutMutex);
        auto nit = g_clutNotes.find(RdClutKey{tbp0, tbw, psm, tw, th});
        if (nit != g_clutNotes.end())
            curClutHash = nit->second.clutHash;
    }

    auto it = g_texCache.find(key);
    if (it != g_texCache.end() && it->second.clutHash == curClutHash) // FEAT-RD3c
    {
        auto bt1Reuse = [&]() { // PROBE-BT1
            if (!g_probeBt1)
                return;
            Bt1Info info;
            {
                std::lock_guard<std::mutex> lk(g_bt1Mutex);
                auto git = g_bt1GlInfo.find(it->second.glId);
                if (git == g_bt1GlInfo.end())
                    return;
                info = git->second;
            }
            if (!info.isBlack || info.w < 16u || info.h < 16u)
                return;
            const bool idx = isIndexedPsm(psm);
            const uint32_t pageSum = bt1PageGenSum(psm, info.w, info.h, tbw, tbp0, cbp, idx);
            bt1Report("reuse", g_bt1CntReuse, false, tex0, tex0Key, psm, info.w, info.h, tbp0, cbp, cpsm,
                      idx, info.clutNoted, curClutHash, info.clutAllZero, 0u, pageSum, true);
        };
        if (it->second.lastFrame == g_texFrame)
        {
            bt1Reuse(); // PROBE-BT1
            return it->second.glId;
        }
        if (!s_checkEveryFrame && (g_texFrame - it->second.lastCheckFrame) < 30u)
        {
            it->second.lastFrame = g_texFrame;
            bt1Reuse(); // PROBE-BT1
            return it->second.glId;
        }
    }

    RdClutNote note{};
    bool haveNote = false;
    if (isIndexedPsm(psm)) // PERF-EX5
    {
        std::lock_guard<std::mutex> lk(g_clutMutex);
        auto nit = g_clutNotes.find(RdClutKey{tbp0, tbw, psm, tw, th});
        if (nit != g_clutNotes.end())
        {
            note = nit->second;
            curClutHash = note.clutHash;
            haveNote = true;
        }
        else
        {
            curClutHash = 0ull;
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

    bool bt1Black = false;   // PROBE-BT1
    bool bt1ClutZero = false; // PROBE-BT1
    if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
    {
        bt1Black = bt1IsRgbaBlack(s_rgba.data(), w, h);
        bt1ClutZero = bt1ClutAllZero(indexed, clut);
        const uint32_t pageSum = bt1PageGenSum(psm, w, h, tbw, tbp0, cbp, indexed);
        bt1Report("fallback", g_bt1CntFallback, true, tex0, tex0Key, psm, w, h, tbp0, cbp, cpsm,
                  indexed, haveNote, curClutHash, bt1ClutZero, 0u, pageSum, bt1Black);
    }

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
        if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
        {
            std::lock_guard<std::mutex> lk(g_bt1Mutex);
            if (g_bt1GlInfo.size() > 16384u)
                g_bt1GlInfo.clear();
            g_bt1GlInfo[it->second.glId] = Bt1Info{w, h, curClutHash, bt1Black, haveNote, bt1ClutZero};
        }
        return it->second.glId;
    }

    const unsigned int glId = rlLoadTexture(s_rgba.data(), static_cast<int>(w), static_cast<int>(h),
                                            PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    g_texCache.emplace(key, RdTexEntry{glId, hash, g_texFrame, g_texFrame, curClutHash}); // FEAT-RD3c
    if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
    {
        std::lock_guard<std::mutex> lk(g_bt1Mutex);
        if (g_bt1GlInfo.size() > 16384u)
            g_bt1GlInfo.clear();
        g_bt1GlInfo[glId] = Bt1Info{w, h, curClutHash, bt1Black, haveNote, bt1ClutZero};
    }
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

    const uint32_t w = 1u << std::min<uint32_t>(tw, 10u);
    const uint32_t h = 1u << std::min<uint32_t>(th, 10u);
    uint32_t fpFirst = 0u, fpLast = 0u, fpCp = 0u; // FEAT-RD5c FEAT-RD28
    computeTexPageRange(psm, w, h, tbw, tbp0, cbp, fpFirst, fpLast, fpCp);

    struct RdpSubScope // FEAT-RD45
    {
        int id;
        uint64_t t0;
        explicit RdpSubScope(int i) : id(i), t0(g_rdpOn ? __rdtsc() : 0ull) {}
        ~RdpSubScope() { if (g_rdpOn) g_rdpNoteSubCyc[id] += __rdtsc() - t0; }
    };

    bool syncedFromGpu = false; // FEAT-RD28
    {
        RdpSubScope subRt(0); // FEAT-RD45: RT checks
        if (ps2xRdActive()) // FEAT-RD28b // FEAT-RD46 // FEAT-RD49
        {
            {
                const uint64_t rtA0 = g_rdpOn ? __rdtsc() : 0ull; // FEAT-RD46
                uint32_t fbF[8], fbL[8];
                const uint32_t nfb = ps2xRdFbPageRanges(fbF, fbL);
                for (uint32_t i = 0; i < nfb; ++i)
                {
                    if (fpFirst <= fbL[i] && fpLast >= fbF[i])
                    {
                        rdRingSlot(g_fbFeedbackRing3d, g_fbFeedbackRing2d, serial).store(serial, std::memory_order_release); // FIX-RD72
                        if (g_rdpOn) g_rdpRtSubCyc[0] += __rdtsc() - rtA0; // FEAT-RD46
                        return;
                    }
                }
                if (g_rdpOn) g_rdpRtSubCyc[0] += __rdtsc() - rtA0; // FEAT-RD46
            }

            uint32_t syncedBytes = 0u;
            if (ps2xVkSyncPagesForCpu(fpFirst, fpLast, &syncedBytes))
            {
                for (uint32_t p = fpFirst; p <= fpLast; ++p)
                    ++g_rdPageGen[p];
                g_rdRtSyncs.fetch_add(1u, std::memory_order_relaxed);
                g_rdRtBytes.fetch_add(syncedBytes, std::memory_order_relaxed);
                syncedFromGpu = true;
            }
            if (isIndexedPsm(psm) && (fpCp < fpFirst || fpCp > fpLast))
            {
                uint32_t clutBytes = 0u;
                if (ps2xVkSyncPagesForCpu(fpCp, fpCp, &clutBytes))
                {
                    ++g_rdPageGen[fpCp];
                    g_rdRtSyncs.fetch_add(1u, std::memory_order_relaxed);
                    g_rdRtBytes.fetch_add(clutBytes, std::memory_order_relaxed);
                    syncedFromGpu = true;
                }
            }
        }
        if (syncedFromGpu)
            ++g_rdVramGen; // FEAT-RD45
    }

    const bool indexed = isIndexedPsm(psm); // FEAT-RD45
    uint64_t curClutHash = 0ull;            // FEAT-RD45
    if (indexed)
    {
        RdpSubScope subClut(2); // FEAT-RD45: CLUT key
        curClutHash = getClutHashFast(clut512);
    }

    const uint64_t tex0Key = tex0 & (0x3FFFFFFFFull | (0xFFFFFFull << 37));
    static thread_local uint32_t t_lastSerial = 0u;
    static thread_local uint64_t t_lastTex0Key = 0ull;
    static thread_local uint64_t t_lastTexa = 0ull;
    static thread_local uint64_t t_lastContentId = 0ull;
    if (syncedFromGpu) // FEAT-RD28
        t_lastContentId = 0ull;
    const uint32_t fastIdx = static_cast<uint32_t>(((tex0Key * 0x9E3779B97F4A7C15ull) ^ (texa * 0xC2B2AE3D27D4EB4Full)) >> 53) & (kFastSets - 1u); // FEAT-RD5c / FEAT-RD45
    {
        RdpSubScope subFast(1); // FEAT-RD45: fast cache lookup
        for (uint32_t w = 0u; w < 2u; ++w)
        {
            RdFastEntry &fe = g_fastCache[fastIdx][w]; // FEAT-RD5c / FEAT-RD45
            if (fe.valid && fe.tex0Key == tex0Key && fe.texa == texa)
            {
                bool clutOk = !fe.indexed || (fe.clutGen == g_rdClutGen);
                if (!clutOk && fe.clutHash == curClutHash)
                {
                    fe.clutGen = g_rdClutGen;
                    clutOk = true;
                }
                if (clutOk)
                {
                    bool pageOk = (fe.vramGen == g_rdVramGen); // FEAT-RD45: O(1) when no VRAM write occurred
                    if (!pageOk)
                    {
                        uint32_t sum = 0;
                        for (uint32_t p = fe.first; p <= fe.last; ++p)
                            sum += g_rdPageGen[p];
                        if (fe.indexed)
                            sum += g_rdPageGen[fe.cp];
                        if (sum == fe.pageSum)
                        {
                            fe.vramGen = g_rdVramGen;
                            pageOk = true;
                        }
                    }
                    if (pageOk)
                    {
                        g_rdNoteCalls.fetch_add(1u, std::memory_order_relaxed);
                        g_rdFastHits.fetch_add(1u, std::memory_order_relaxed);
                        if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
                            bt1ReportReuseCid(fe.contentId, tex0, tex0Key, psm, tbp0, tbw, cbp, cpsm, indexed, serial);
                        writeSerialRing(serial, fe.contentId);
                        return;
                    }
                }
            }
        }
    }
    if (serial == t_lastSerial && tex0Key == t_lastTex0Key && texa == t_lastTexa && t_lastContentId != 0ull)
    {
        if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
            bt1ReportReuseCid(t_lastContentId, tex0, tex0Key, psm, tbp0, tbw, cbp, cpsm, indexed, serial);
        writeSerialRing(serial, t_lastContentId);
        return;
    }

    g_rdNoteCalls.fetch_add(1u, std::memory_order_relaxed); // FEAT-RD5b

    // FEAT-RD5b: cheap key from VRAM page write generations + CLUT hash; full decode only when unseen.
    static std::unordered_map<uint64_t, uint64_t> s_keyToContent; // GS thread only
    uint64_t quickKey = 0ull;
    {
        RdpSubScope subQk(3); // FEAT-RD45: quick key
        uint64_t gh = 0xcbf29ce484222325ull;
        for (uint32_t p = fpFirst; p <= fpLast; ++p)
            gh = (gh ^ g_rdPageGen[p]) * 0x100000001b3ull;
        if (indexed)
        {
            gh = (gh ^ g_rdPageGen[fpCp]) * 0x100000001b3ull;
            if (clut512)
                gh = fnv1a64(&curClutHash, sizeof(curClutHash), gh); // FEAT-RD45: O(1) cached CLUT hash
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
            storeFast(fastIdx, tex0Key, texa, itKey->second, fpFirst, fpLast, fpCp, indexed, curClutHash); // FEAT-RD5c / FEAT-RD45
            if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
                bt1ReportReuseCid(itKey->second, tex0, tex0Key, psm, tbp0, tbw, cbp, cpsm, indexed, serial);
            writeSerialRing(serial, itKey->second);
            return;
        }
    }
    // FEAT-RD5d: raw VRAM CRC key (no per-texel reads); falls through to full decode only if unseen.
    static std::unordered_map<uint64_t, uint64_t> s_rawToContent; // GS thread only
    uint64_t rawKey = 0ull;
    {
        RdpSubScope subCrc(4); // FEAT-RD45: raw VRAM CRC
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
        if (indexed)
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
            storeFast(fastIdx, tex0Key, texa, itRaw->second, fpFirst, fpLast, fpCp, indexed, curClutHash); // FEAT-RD45
            if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
                bt1ReportReuseCid(itRaw->second, tex0, tex0Key, psm, tbp0, tbw, cbp, cpsm, indexed, serial);
            writeSerialRing(serial, itRaw->second);
            return;
        }
    }
    RdpSubScope subDec(5); // FEAT-RD45: full decode + other
    g_rdFullDecodes.fetch_add(1u, std::memory_order_relaxed);

    GSMem::TexturePageCache pageCache;
    uint32_t clut[256] = {};
    uint64_t clutHash = 0ull;
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

    bool bt1DecodedNew = false; // PROBE-BT1
    bool bt1Black = false;      // PROBE-BT1
    {
        std::lock_guard<std::mutex> lk(g_contentMutex);
        if (g_contentMap.find(contentId) == g_contentMap.end())
        {
            RdContentEntry entry;
            entry.w = w;
            entry.h = h;
            entry.psm = psm; // FEAT-RD6
            entry.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            auto *dst = reinterpret_cast<uint32_t *>(entry.rgba.data());
            for (size_t i = 0; i < t_raw.size(); ++i)
                dst[i] = decodeTexel(t_raw[i], psm, texa, indexed, clut);
            if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
            {
                bt1DecodedNew = true;
                bt1Black = bt1IsRgbaBlack(entry.rgba.data(), w, h);
            }
            g_contentMap.emplace(contentId, std::move(entry));
        }
    }
    if (g_probeBt1 && w >= 16u && h >= 16u) // PROBE-BT1
    {
        if (bt1DecodedNew)
        {
            const bool bt1ClutZero = bt1ClutAllZero(indexed, clut);
            const bool clutNoted = (clut512 != nullptr);
            {
                std::lock_guard<std::mutex> lk(g_bt1Mutex);
                if (g_bt1CidInfo.size() > 16384u)
                    g_bt1CidInfo.clear();
                g_bt1CidInfo[contentId] = Bt1Info{w, h, clutHash, bt1Black, clutNoted, bt1ClutZero};
            }
            uint32_t pageSum = 0u;
            for (uint32_t p = fpFirst; p <= fpLast; ++p)
                pageSum += g_rdPageGen[p];
            if (indexed)
                pageSum += g_rdPageGen[fpCp];
            bt1Report("note", g_bt1CntNote, true, tex0, tex0Key, psm, w, h, tbp0, cbp, cpsm,
                      indexed, clutNoted, clutHash, bt1ClutZero, serial, pageSum, bt1Black);
        }
        else
        {
            bt1ReportReuseCid(contentId, tex0, tex0Key, psm, tbp0, tbw, cbp, cpsm, indexed, serial);
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
    storeFast(fastIdx, tex0Key, texa, contentId, fpFirst, fpLast, fpCp, indexed, curClutHash); // FEAT-RD5c / FEAT-RD45
    writeSerialRing(serial, contentId);
}

bool ps2xRdIsFbFeedback(uint32_t serial) // FEAT-RD28b
{
    if (serial == 0u)
        return false;
    return rdRingSlot(g_fbFeedbackRing3d, g_fbFeedbackRing2d, serial).load(std::memory_order_acquire) == serial; // FIX-RD72
}

uint64_t ps2xRdLookupContentId(uint32_t serial) // FEAT-RD5
{
    if (serial == 0u)
        return 0ull;
    const RdSerialSlot &slot = rdRingSlot(g_serialRing3d, g_serialRing2d, serial); // FIX-RD72
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
            if (g_probeBt1) // PROBE-BT1
            {
                const uint32_t tbp0 = static_cast<uint32_t>(tex0 & 0x3FFFull);
                const uint32_t tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Full);
                const uint8_t psm = static_cast<uint8_t>((tex0 >> 20) & 0x3Full);
                const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFull);
                const uint8_t cpsm = static_cast<uint8_t>((tex0 >> 51) & 0xFull);
                const uint64_t tex0Key = tex0 & (0x3FFFFFFFFull | (0xFFFFFFull << 37));
                bt1ReportReuseCid(cid, tex0, tex0Key, psm, tbp0, tbw, cbp, cpsm, isIndexedPsm(psm), serial);
            }
            return it->second.glId;
        }
        uint32_t w = 0u, h = 0u, cpsmFeat = 0u;
        std::vector<uint8_t> rgbaCopy;
        {
            std::lock_guard<std::mutex> lk(g_contentMutex);
            auto cit = g_contentMap.find(cid);
            if (cit != g_contentMap.end())
            {
                w = cit->second.w;
                h = cit->second.h;
                cpsmFeat = cit->second.psm;
                rgbaCopy = cit->second.rgba;
            }
        }
        if (w > 0u && h > 0u && !rgbaCopy.empty())
        {
            const unsigned int glId = rd6Upload(cid, w, h, cpsmFeat, rgbaCopy); // FEAT-RD6
            g_glByContentId.emplace(cid, RdContentGl{glId, g_texFrame});
            ++g_decodesSinceLog;
            ++g_exactSinceLog;
            return glId;
        }
    }
    if (g_probeBt1) // PROBE-BT2
    {
        const int kind = g_bt2Kind;
        g_bt2Kind = -1;
        const uint32_t kIdx = (kind == 0) ? 0u : (kind == 1 ? 1u : 2u);
        const uint32_t slotSerial = (serial != 0u) ? rdRingSlot(g_serialRing3d, g_serialRing2d, serial).serial.load(std::memory_order_acquire) : 0u; // FIX-RD72
        uint32_t mIdx = 4u;
        char missChar = 'E';
        if (serial == 0u)
        {
            mIdx = 0u;
            missChar = 'A';
        }
        else if (cid != 0ull)
        {
            mIdx = 3u;
            missChar = 'D';
        }
        else if (slotSerial != serial)
        {
            mIdx = 1u;
            missChar = 'B';
        }
        else if (slotSerial == serial)
        {
            mIdx = 2u;
            missChar = 'C';
        }
        g_bt2Miss[kIdx][mIdx].fetch_add(1u, std::memory_order_relaxed);
        if (g_bt2Lines.load(std::memory_order_relaxed) < 40u &&
            g_bt2Lines.fetch_add(1u, std::memory_order_relaxed) < 40u)
        {
            const uint8_t psm = static_cast<uint8_t>((tex0 >> 20) & 0x3Full);
            const long long diff = static_cast<long long>(serial) - static_cast<long long>(slotSerial);
            ps2xSafeLog("[rdbt2] miss=%c kind=%d serial=%u slotSerial=%u diff=%lld contentId=%llx tex0=%llx psm=%u indexed=%d\n",
                        missChar, kind, static_cast<unsigned>(serial), static_cast<unsigned>(slotSerial),
                        diff, static_cast<unsigned long long>(cid), static_cast<unsigned long long>(tex0),
                        static_cast<unsigned>(psm), isIndexedPsm(psm) ? 1 : 0);
        }
    }
    ++g_fallbackSinceLog;
    return ps2xRdGetTexture(tex0, texa);
}

void ps2xRdNoteState(uint32_t serial, uint64_t prim, uint64_t alpha, uint64_t test, uint64_t frame,
                     uint64_t zbuf, uint64_t tex0, uint64_t tex1, uint64_t clamp, uint64_t texa, uint64_t fba) // FEAT-RD17
{
    if (serial == 0u)
        return;
    RdStateSlot &slot = g_stateRing[serial & (kRdRingSize - 1u)];
    slot.serial.store(0u, std::memory_order_release);
    slot.prim.store(prim, std::memory_order_relaxed);
    slot.alpha.store(alpha, std::memory_order_relaxed);
    slot.test.store(test, std::memory_order_relaxed);
    slot.frame.store(frame, std::memory_order_relaxed);
    slot.zbuf.store(zbuf, std::memory_order_relaxed);
    slot.tex0.store(tex0, std::memory_order_relaxed);
    slot.tex1.store(tex1, std::memory_order_relaxed);
    slot.clamp.store(clamp, std::memory_order_relaxed);
    slot.texa.store(texa, std::memory_order_relaxed);
    slot.fba.store(fba, std::memory_order_relaxed);
    slot.serial.store(serial, std::memory_order_release);
}

void ps2xRdNoteXyoff(uint32_t serial, uint64_t xyoff) // FEAT-RD21
{
    if (serial == 0u)
        return;
    g_stateRing[serial & (kRdRingSize - 1u)].xyoff.store(xyoff, std::memory_order_relaxed);
}

void ps2xRdNoteScissor(uint32_t serial, uint64_t scis) // FEAT-RD22
{
    if (serial == 0u)
        return;
    g_stateRing[serial & (kRdRingSize - 1u)].scis.store(scis, std::memory_order_relaxed);
}

bool ps2xRdLookupScissor(uint32_t serial, uint64_t &scis) // FEAT-RD22
{
    if (serial == 0u)
        return false;
    const RdStateSlot &slot = g_stateRing[serial & (kRdRingSize - 1u)];
    if (slot.serial.load(std::memory_order_acquire) != serial)
        return false;
    scis = slot.scis.load(std::memory_order_relaxed);
    return true;
}

bool ps2xRdLookupXyoff(uint32_t serial, uint64_t &xyoff) // FEAT-RD21
{
    if (serial == 0u)
        return false;
    const RdStateSlot &slot = g_stateRing[serial & (kRdRingSize - 1u)];
    if (slot.serial.load(std::memory_order_acquire) != serial)
        return false;
    xyoff = slot.xyoff.load(std::memory_order_relaxed);
    return true;
}

bool ps2xRdLookupState(uint32_t serial, uint64_t &prim, uint64_t &alpha, uint64_t &test, uint64_t &frame,
                       uint64_t &zbuf, uint64_t &tex0, uint64_t &tex1, uint64_t &clamp, uint64_t &texa, uint64_t &fba) // FEAT-RD17
{
    if (serial == 0u)
        return false;
    const RdStateSlot &slot = g_stateRing[serial & (kRdRingSize - 1u)];
    if (slot.serial.load(std::memory_order_acquire) != serial)
        return false;
    prim = slot.prim.load(std::memory_order_relaxed);
    alpha = slot.alpha.load(std::memory_order_relaxed);
    test = slot.test.load(std::memory_order_relaxed);
    frame = slot.frame.load(std::memory_order_relaxed);
    zbuf = slot.zbuf.load(std::memory_order_relaxed);
    tex0 = slot.tex0.load(std::memory_order_relaxed);
    tex1 = slot.tex1.load(std::memory_order_relaxed);
    clamp = slot.clamp.load(std::memory_order_relaxed);
    texa = slot.texa.load(std::memory_order_relaxed);
    fba = slot.fba.load(std::memory_order_relaxed);
    return slot.serial.load(std::memory_order_acquire) == serial;
}
