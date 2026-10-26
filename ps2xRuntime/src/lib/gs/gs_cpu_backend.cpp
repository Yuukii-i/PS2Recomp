#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include "probe_ae.h"
namespace probe_ae {
    void bhLog(const char *fmt, ...);
}
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <filesystem>
#include <fstream>

#include <chrono>

std::atomic<uint64_t> g_profGsUs{0};
std::atomic<uint64_t> g_profXferUs{0};
std::atomic<uint64_t> g_profPrims{0};
std::atomic<uint64_t> g_profPixels{0};

static inline bool isVerboseLog()
{
    static int s_verbose = -1;
    if (s_verbose == -1)
    {
        const char *v = std::getenv("PS2X_VERBOSE_LOG");
        s_verbose = (v && (v[0] == '1' || v[0] == 't')) ? 1 : 0;
    }
    return s_verbose == 1;
}

// --- Task K: Offline Oracle & Diagnostic Helpers ---
static uint32_t calculateCrc32(const uint8_t *data, size_t length)
{
    static uint32_t table[256];
    static bool haveTable = false;
    if (!haveTable)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        haveTable = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
    {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t adler32(const uint8_t *data, size_t length)
{
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0; i < length; ++i)
    {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16u) | s1;
}

void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename)
{
    std::vector<uint8_t> raw;
    raw.reserve((w * 4u + 1u) * h);
    for (uint32_t y = 0; y < h; ++y)
    {
        raw.push_back(0); // filter byte
        raw.insert(raw.end(), rgba + y * w * 4u, rgba + (y + 1u) * w * 4u);
    }

    std::vector<uint8_t> zstream;
    zstream.push_back(0x78);
    zstream.push_back(0x01);

    size_t pos = 0;
    while (pos < raw.size())
    {
        uint16_t chunkLen = static_cast<uint16_t>(std::min<size_t>(32768, raw.size() - pos));
        uint8_t isFinal = (pos + chunkLen >= raw.size()) ? 1u : 0u;
        zstream.push_back(isFinal);
        uint16_t nlen = chunkLen ^ 0xFFFFu;
        zstream.push_back(static_cast<uint8_t>(chunkLen & 0xFFu));
        zstream.push_back(static_cast<uint8_t>((chunkLen >> 8u) & 0xFFu));
        zstream.push_back(static_cast<uint8_t>(nlen & 0xFFu));
        zstream.push_back(static_cast<uint8_t>((nlen >> 8u) & 0xFFu));
        zstream.insert(zstream.end(), raw.data() + pos, raw.data() + pos + chunkLen);
        pos += chunkLen;
    }
    uint32_t adler = adler32(raw.data(), raw.size());
    zstream.push_back(static_cast<uint8_t>((adler >> 24u) & 0xFFu));
    zstream.push_back(static_cast<uint8_t>((adler >> 16u) & 0xFFu));
    zstream.push_back(static_cast<uint8_t>((adler >> 8u) & 0xFFu));
    zstream.push_back(static_cast<uint8_t>(adler & 0xFFu));

    std::vector<uint8_t> png;
    const uint8_t sig[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    png.insert(png.end(), sig, sig + sizeof(sig));

    // IHDR
    uint8_t ihdrData[13];
    ihdrData[0] = static_cast<uint8_t>((w >> 24u) & 0xFFu);
    ihdrData[1] = static_cast<uint8_t>((w >> 16u) & 0xFFu);
    ihdrData[2] = static_cast<uint8_t>((w >> 8u) & 0xFFu);
    ihdrData[3] = static_cast<uint8_t>(w & 0xFFu);
    ihdrData[4] = static_cast<uint8_t>((h >> 24u) & 0xFFu);
    ihdrData[5] = static_cast<uint8_t>((h >> 16u) & 0xFFu);
    ihdrData[6] = static_cast<uint8_t>((h >> 8u) & 0xFFu);
    ihdrData[7] = static_cast<uint8_t>(h & 0xFFu);
    ihdrData[8] = 8; // bit depth
    ihdrData[9] = 6; // color type: RGBA
    ihdrData[10] = 0; // compression
    ihdrData[11] = 0; // filter
    ihdrData[12] = 0; // interlace

    auto writeChunk = [&](const char *type, const uint8_t *data, size_t len)
    {
        uint32_t ulen = static_cast<uint32_t>(len);
        png.push_back(static_cast<uint8_t>((ulen >> 24u) & 0xFFu));
        png.push_back(static_cast<uint8_t>((ulen >> 16u) & 0xFFu));
        png.push_back(static_cast<uint8_t>((ulen >> 8u) & 0xFFu));
        png.push_back(static_cast<uint8_t>(ulen & 0xFFu));
        size_t typePos = png.size();
        png.insert(png.end(), type, type + 4);
        if (len > 0)
            png.insert(png.end(), data, data + len);
        uint32_t chunkCrc = calculateCrc32(png.data() + typePos, len + 4);
        png.push_back(static_cast<uint8_t>((chunkCrc >> 24u) & 0xFFu));
        png.push_back(static_cast<uint8_t>((chunkCrc >> 16u) & 0xFFu));
        png.push_back(static_cast<uint8_t>((chunkCrc >> 8u) & 0xFFu));
        png.push_back(static_cast<uint8_t>(chunkCrc & 0xFFu));
    };

    writeChunk("IHDR", ihdrData, sizeof(ihdrData));
    writeChunk("IDAT", zstream.data(), zstream.size());
    writeChunk("IEND", nullptr, 0);

    std::filesystem::create_directories("logs/gs");
    FILE *fp = std::fopen(filename, "wb");
    if (fp)
    {
        std::fwrite(png.data(), 1, png.size(), fp);
        std::fclose(fp);
    }
}

// Global instrumentation state
static GSCpuBackend *s_cpuBackendInstance = nullptr;
static uint8_t s_lastSeenCld = 0u;
static uint16_t s_savedClut[512] = {0};
static uint32_t s_savedClutCbp[2] = {0};
static int s_clut32Swizzle = -1;
static int s_clutDrawtimeReload = -1;
static int s_presentFallback = -1;
static int s_uploadDump = -1;
static int s_texDump = -1;
static uint32_t s_uploadSeq = 0u;
static std::vector<uint8_t> s_currentUploadPayload;
static GSTransferCommand s_currentUploadCommand{};
static bool s_inUpload = false;

static int s_primLogFrom = -1;
static int s_primLogMax = -1;
static int s_drawStepDump = -1;

struct DumpedTexKey {
    uint32_t tbp0;
    uint32_t tbw;
    uint8_t psm;
    uint32_t tw;
    uint32_t th;
    uint32_t cbp;
    uint8_t cpsm;
    uint8_t csm;
    uint8_t csa;
    uint32_t clutHash;
};
static std::vector<DumpedTexKey> s_dumpedTexKeys;


using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outHeight > 448u && outHeight <= 896u)
        {
            outHeight = (outHeight + 1u) / 2u;
        }
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

#include <atomic>
#include <vector>
#include <utility>

static std::atomic<uint32_t> s_diagPrimsPerFbp[512]{};
void gsRecordPrimitiveFbp(uint32_t fbp)
{
    if (fbp < 512u)
    {
        s_diagPrimsPerFbp[fbp].fetch_add(1u, std::memory_order_relaxed);
    }
}

void gsGetAndResetPrimsPerFbp(std::vector<std::pair<uint32_t, uint32_t>> &out)
{
    out.clear();
    for (uint32_t i = 0; i < 512u; ++i)
    {
        uint32_t count = s_diagPrimsPerFbp[i].exchange(0u, std::memory_order_relaxed);
        if (count > 0u)
        {
            out.emplace_back(i, count);
        }
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    s_cpuBackendInstance = this;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    uint64_t pxBefore = g_profPixels.load(std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    DrawPrimitive(batch);
    const auto t1 = std::chrono::steady_clock::now();
    uint64_t elapsedUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    g_profGsUs.fetch_add(elapsedUs, std::memory_order_relaxed);
    g_profPrims.fetch_add(1u, std::memory_order_relaxed);
    probe_ae::onBhGsRasterTime(elapsedUs);
    uint64_t pxDrawn = g_profPixels.load(std::memory_order_relaxed) - pxBefore;
    probe_ae::onBhGsPacket(elapsedUs, static_cast<uint32_t>(batch.state.prim.type), static_cast<uint32_t>(pxDrawn),
                           batch.state.prim.tme != 0, batch.state.prim.abe != 0,
                           1u << batch.state.context.tex0.tw, 1u << batch.state.context.tex0.th);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    s_lastSeenCld = tex0.cld;
    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const uint32_t effectiveCbp = (tex0.cbp != 0u) ? tex0.cbp : m_clutCbp[0];
    if (effectiveCbp == 0u)
        return;

    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            if (s_clut32Swizzle == -1)
        {
            const char *env = std::getenv("PS2X_GS_CLUT32_SWIZZLE");
            s_clut32Swizzle = (env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0)) ? 0 : 1;
        }
        const bool doSwizzle = !fourBit && (sixteenBit || (thirtyTwoBit && s_clut32Swizzle == 1));
        const uint32_t sourceIndex = doSwizzle ? swizzleClutIndexCSM1(entry) : entry;
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, effectiveCbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
    std::memcpy(s_savedClut, m_clut.data(), sizeof(s_savedClut));
    s_savedClutCbp[0] = m_clutCbp[0];
    s_savedClutCbp[1] = m_clutCbp[1];
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    probe_ae::onBxDrawPrimitive(batch);
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex == 512u)
        {
            extern void ps2xSafeLog(const char *fmt, ...);
            ps2xSafeLog("[log] cap reached for tag gs:prim at idx 512\n");
            std::fflush(stderr);
        }
        gsRecordPrimitiveFbp(ctx.frame.fbp);
        if (isVerboseLog() && primitiveIndex < 512u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    static std::atomic<uint32_t> s_globalPrimIndex{0};
    uint32_t currentPrimIdx = s_globalPrimIndex.fetch_add(1u, std::memory_order_relaxed);

    if (s_primLogFrom == -1)
    {
        const char *envFrom = std::getenv("PS2X_GS_PRIM_LOG_FROM");
        s_primLogFrom = envFrom ? std::atoi(envFrom) : 0;
        const char *envMax = std::getenv("PS2X_GS_PRIM_LOG_MAX");
        s_primLogMax = envMax ? std::atoi(envMax) : 0;
    }

    if (s_primLogMax > 0 &&
        currentPrimIdx >= static_cast<uint32_t>(s_primLogFrom) &&
        currentPrimIdx < static_cast<uint32_t>(s_primLogFrom + s_primLogMax))
    {
        const char *primName = "UNKNOWN";
        switch (state.prim.type)
        {
        case GS_PRIM_POINT: primName = "POINT"; break;
        case GS_PRIM_LINE: primName = "LINE"; break;
        case GS_PRIM_LINESTRIP: primName = "LINESTRIP"; break;
        case GS_PRIM_TRIANGLE: primName = "TRIANGLE"; break;
        case GS_PRIM_TRISTRIP: primName = "TRISTRIP"; break;
        case GS_PRIM_TRIFAN: primName = "TRIFAN"; break;
        case GS_PRIM_SPRITE: primName = "SPRITE"; break;
        }
        int ofx = ctx.xyoffset.ofx >> 4;
        int ofy = ctx.xyoffset.ofy >> 4;
        float x0 = batch.vertices.size() > 0 ? (batch.vertices[0].x - static_cast<float>(ofx)) : 0.f;
        float y0 = batch.vertices.size() > 0 ? (batch.vertices[0].y - static_cast<float>(ofy)) : 0.f;
        float x1 = batch.vertices.size() > 1 ? (batch.vertices[1].x - static_cast<float>(ofx)) : 0.f;
        float y1 = batch.vertices.size() > 1 ? (batch.vertices[1].y - static_cast<float>(ofy)) : 0.f;

        if (isVerboseLog()) std::fprintf(stderr, "[GS:PRIM] idx=%u type=%s(0x%x) fbp=%u fbw=%u psm=0x%x tme=%u abe=%u fst=%u "
                             "tbp0=%u tbw=%u tpsm=0x%x tw=%u th=%u cbp=%u cpsm=0x%x csm=%u csa=%u "
                             "alpha=0x%llx test=0x%llx rect=(%.1f,%.1f)-(%.1f,%.1f) v1_rgba=(%u,%u,%u,%u)\n",
                     currentPrimIdx, primName, static_cast<unsigned>(state.prim.type),
                     ctx.frame.fbp, ctx.frame.fbw, static_cast<unsigned>(ctx.frame.psm),
                     state.prim.tme, state.prim.abe, state.prim.fst,
                     ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), static_cast<unsigned>(ctx.tex0.psm),
                     static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th),
                     (ctx.tex0.cbp != 0u) ? ctx.tex0.cbp : m_clutCbp[0],
                     static_cast<unsigned>(ctx.tex0.cpsm), static_cast<unsigned>(ctx.tex0.csm), static_cast<unsigned>(ctx.tex0.csa),
                     static_cast<unsigned long long>(ctx.alpha), static_cast<unsigned long long>(ctx.test),
                     x0, y0, x1, y1,
                     batch.vertices.size() > 1 ? batch.vertices[1].r : (batch.vertices.size() > 0 ? batch.vertices[0].r : 0),
                     batch.vertices.size() > 1 ? batch.vertices[1].g : (batch.vertices.size() > 0 ? batch.vertices[0].g : 0),
                     batch.vertices.size() > 1 ? batch.vertices[1].b : (batch.vertices.size() > 0 ? batch.vertices[0].b : 0),
                     batch.vertices.size() > 1 ? batch.vertices[1].a : (batch.vertices.size() > 0 ? batch.vertices[0].a : 0));
        std::fflush(stderr);
    }

    if (s_clutDrawtimeReload == -1)
    {
        const char *env = std::getenv("PS2X_GS_CLUT_DRAWTIME_RELOAD");
        s_clutDrawtimeReload = (env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0)) ? 0 : 1;
    }
    if (state.prim.tme && (isFourBitIndexedPsm(ctx.tex0.psm) || isEightBitIndexedPsm(ctx.tex0.psm)))
    {
        if (s_clutDrawtimeReload != 0 && (ctx.tex0.cbp != 0u || m_clutCbp[0] != 0u))
        {
            LoadClutUnlocked(ctx.tex0, state.texclut);
        }
    }

    if (s_texDump == -1)
    {
        const char *env = std::getenv("PS2X_TEX_DUMP");
        if (!env) env = std::getenv("PS2X_GS_TEXDUMP");
        s_texDump = (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0)) ? 1 : 0;
    }
    if ((s_texDump == 1 || ctx.tex0.tbp0 == 10752u) && state.prim.tme && s_dumpedTexKeys.size() < 16u)
    {
        const uint32_t effectiveCbp = (ctx.tex0.cbp != 0u) ? ctx.tex0.cbp : m_clutCbp[0];
        uint32_t clutHash = calculateCrc32(reinterpret_cast<const uint8_t*>(m_clut.data()), sizeof(m_clut));
        uint32_t tw_px = 1u << ctx.tex0.tw;
        uint32_t th_px = 1u << ctx.tex0.th;

        bool alreadyDumped = false;
        for (const auto &k : s_dumpedTexKeys)
        {
            if (k.tbp0 == ctx.tex0.tbp0 && k.tbw == ctx.tex0.tbw && k.psm == ctx.tex0.psm &&
                k.tw == ctx.tex0.tw && k.th == ctx.tex0.th && k.cbp == effectiveCbp &&
                k.cpsm == ctx.tex0.cpsm && k.csm == ctx.tex0.csm && k.csa == ctx.tex0.csa &&
                k.clutHash == clutHash)
            {
                alreadyDumped = true;
                break;
            }
        }
        if (!alreadyDumped)
        {
            s_dumpedTexKeys.push_back({ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, ctx.tex0.tw, ctx.tex0.th,
                                       effectiveCbp, ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, clutHash});
            uint32_t seq = static_cast<uint32_t>(s_dumpedTexKeys.size());
            std::vector<uint8_t> sampledRgba(tw_px * th_px * 4u, 0u);
            for (uint32_t sy = 0; sy < th_px; ++sy)
            {
                for (uint32_t sx = 0; sx < tw_px; ++sx)
                {
                    uint32_t rgba = SampleTexture(state, 0.f, 0.f, 1.f, static_cast<uint16_t>(sx * 16u), static_cast<uint16_t>(sy * 16u));
                    sampledRgba[(sy * tw_px + sx) * 4u + 0] = static_cast<uint8_t>(rgba & 0xFFu);
                    sampledRgba[(sy * tw_px + sx) * 4u + 1] = static_cast<uint8_t>((rgba >> 8u) & 0xFFu);
                    sampledRgba[(sy * tw_px + sx) * 4u + 2] = static_cast<uint8_t>((rgba >> 16u) & 0xFFu);
                    sampledRgba[(sy * tw_px + sx) * 4u + 3] = static_cast<uint8_t>((rgba >> 24u) & 0xFFu);
                }
            }
            std::filesystem::create_directories("frames/tex");
            char fn[256];
            std::snprintf(fn, sizeof(fn), "frames/tex/tex_%u_tbp%u_psm0x%02x_%ux%u.png",
                          seq, ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.psm), tw_px, th_px);
            writeUncompressedPng(tw_px, th_px, sampledRgba.data(), fn);

            // Dump CLUT 16x16
            std::vector<uint8_t> clutRgba(16u * 16u * 4u, 0u);
            for (uint32_t ci = 0; ci < 256u; ++ci)
            {
                uint32_t raw = (ctx.tex0.cpsm == GS_PSM_CT16 || ctx.tex0.cpsm == GS_PSM_CT16S)
                    ? Rgba5551ToRgba8888(m_clut[ci])
                    : (static_cast<uint32_t>(m_clut[ci]) | (static_cast<uint32_t>(m_clut[ci + 256u]) << 16u));
                clutRgba[ci * 4u + 0] = static_cast<uint8_t>(raw & 0xFFu);
                clutRgba[ci * 4u + 1] = static_cast<uint8_t>((raw >> 8u) & 0xFFu);
                clutRgba[ci * 4u + 2] = static_cast<uint8_t>((raw >> 16u) & 0xFFu);
                clutRgba[ci * 4u + 3] = static_cast<uint8_t>((raw >> 24u) & 0xFFu);
            }
            char clutFn[256];
            std::snprintf(clutFn, sizeof(clutFn), "frames/tex/clut_%u_cbp%u_cpsm0x%02x.png",
                          seq, effectiveCbp, static_cast<unsigned>(ctx.tex0.cpsm));
            writeUncompressedPng(16u, 16u, clutRgba.data(), clutFn);

            const bool csm1Remapped = (ctx.tex0.csm == 0 && (ctx.tex0.cpsm == GS_PSM_CT16 || ctx.tex0.cpsm == GS_PSM_CT16S));
            std::fprintf(stderr, "[GS:TEXDUMP] [%u/16] tex=(tbp0=%u, tbw=%u, psm=0x%02x, tw=%u, th=%u, %ux%u) cbp=%u csa=%u csm=%u csm1_remap=%d clutHash=0x%08x -> %s\n",
                         seq, ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), static_cast<unsigned>(ctx.tex0.psm),
                         static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th), tw_px, th_px,
                         effectiveCbp, static_cast<unsigned>(ctx.tex0.csa), static_cast<unsigned>(ctx.tex0.csm),
                         csm1Remapped ? 1 : 0, clutHash, fn);
            std::fflush(stderr);
        }
    }

    if (probe_ae::isEnabled() && !batch.vertices.empty())
    {
        int ofx = ctx.xyoffset.ofx >> 4;
        int ofy = ctx.xyoffset.ofy >> 4;
        int minX = static_cast<int>(std::floor(batch.vertices[0].x - static_cast<float>(ofx)));
        int maxX = static_cast<int>(std::ceil(batch.vertices[0].x - static_cast<float>(ofx)));
        int minY = static_cast<int>(std::floor(batch.vertices[0].y - static_cast<float>(ofy)));
        int maxY = static_cast<int>(std::ceil(batch.vertices[0].y - static_cast<float>(ofy)));
        for (size_t i = 1; i < batch.vertices.size(); ++i)
        {
            int vx0 = static_cast<int>(std::floor(batch.vertices[i].x - static_cast<float>(ofx)));
            int vx1 = static_cast<int>(std::ceil(batch.vertices[i].x - static_cast<float>(ofx)));
            int vy0 = static_cast<int>(std::floor(batch.vertices[i].y - static_cast<float>(ofy)));
            int vy1 = static_cast<int>(std::ceil(batch.vertices[i].y - static_cast<float>(ofy)));
            if (vx0 < minX) minX = vx0;
            if (vx1 > maxX) maxX = vx1;
            if (vy0 < minY) minY = vy0;
            if (vy1 > maxY) maxY = vy1;
        }

        if (maxY >= 416)
        {
            const char *primName = "UNKNOWN";
            switch (state.prim.type)
            {
            case GS_PRIM_POINT: primName = "POINT"; break;
            case GS_PRIM_LINE: primName = "LINE"; break;
            case GS_PRIM_LINESTRIP: primName = "LINESTRIP"; break;
            case GS_PRIM_TRIANGLE: primName = "TRIANGLE"; break;
            case GS_PRIM_TRISTRIP: primName = "TRISTRIP"; break;
            case GS_PRIM_TRIFAN: primName = "TRIFAN"; break;
            case GS_PRIM_SPRITE: primName = "SPRITE"; break;
            }
            char texBuf[32];
            if (state.prim.tme)
            {
                std::snprintf(texBuf, sizeof(texBuf), "%u/%u/%u/%u",
                              ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.psm),
                              static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th));
            }
            else
            {
                std::strcpy(texBuf, "none");
            }
            const auto &v = batch.vertices.back();
            uint32_t rgba = static_cast<uint32_t>(v.r) |
                            (static_cast<uint32_t>(v.g) << 8u) |
                            (static_cast<uint32_t>(v.b) << 16u) |
                            (static_cast<uint32_t>(v.a) << 24u);
            probe_ae::onAyPrim(primName, minX, minY, maxX, maxY, rgba, texBuf, ctx.alpha, ctx.test, ctx.frame.fbp);
        }
    }

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }

    if (s_drawStepDump == -1)
    {
        const char *envStep = std::getenv("PS2X_DRAW_STEP_DUMP");
        s_drawStepDump = envStep ? std::atoi(envStep) : -2;
    }
    if (s_drawStepDump >= 0 && currentPrimIdx == static_cast<uint32_t>(s_drawStepDump))
    {
        std::filesystem::create_directories("logs/gs");
        char stepFn[256];
        std::snprintf(stepFn, sizeof(stepFn), "logs/gs/step_draw_%u.png", currentPrimIdx);
        std::vector<uint8_t> snapRgba(640u * 512u * 4u, 0u);
        const uint32_t fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        for (uint32_t py = 0; py < 512u; ++py)
        {
            for (uint32_t px = 0; px < 640u; ++px)
            {
                uint32_t raw = ReadVramUnlocked(ctx.frame.psm, fbp, fbw, px, py);
                uint32_t c = (ctx.frame.psm == GS_PSM_CT32) ? raw :
                             ((ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S) ? Rgba5551ToRgba8888(raw) : raw);
                snapRgba[(py * 640u + px) * 4u + 0] = static_cast<uint8_t>(c & 0xFFu);
                snapRgba[(py * 640u + px) * 4u + 1] = static_cast<uint8_t>((c >> 8u) & 0xFFu);
                snapRgba[(py * 640u + px) * 4u + 2] = static_cast<uint8_t>((c >> 16u) & 0xFFu);
                snapRgba[(py * 640u + px) * 4u + 3] = static_cast<uint8_t>((c >> 24u) & 0xFFu);
            }
        }
        writeUncompressedPng(640u, 512u, snapRgba.data(), stepFn);
        std::fprintf(stderr, "[GS:STEP_DUMP] Prim %u dumped framebuffer fbp=%u to %s\n", currentPrimIdx, ctx.frame.fbp, stepFn);
        std::fflush(stderr);
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;
    g_profPixels.fetch_add(1u, std::memory_order_relaxed);

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    const uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

static std::atomic<uint64_t> s_cmFastTaken{0};
static std::atomic<uint64_t> s_cmGeneric{0};
static std::atomic<uint64_t> s_cmChecked{0};
static std::atomic<uint64_t> s_cmFastOk{0};
static std::atomic<uint64_t> s_cmMismPx{0};
static std::atomic<uint32_t> s_cmMismLogged{0};
static auto s_cmLastSecLog = std::chrono::steady_clock::now();

static std::atomic<uint64_t> s_cnBigSprites{0};
static std::atomic<uint64_t> s_cnPass{0};
static std::atomic<uint64_t> s_cnFailA{0};
static std::atomic<uint64_t> s_cnFailB{0};
static std::atomic<uint64_t> s_cnFailC{0};
static std::atomic<uint64_t> s_cnFailD{0};
static std::atomic<uint64_t> s_cnFailE{0};
static std::atomic<uint64_t> s_cnFailF{0};
static std::atomic<uint64_t> s_cnFailG{0};
static std::atomic<uint64_t> s_cnFailH{0};
static std::atomic<uint64_t> s_cnFailI{0};
static std::atomic<uint64_t> s_cnFailJ{0};
static std::atomic<uint64_t> s_cnFailK{0};
static std::atomic<uint64_t> s_cnFailL{0};
static std::atomic<uint64_t> s_cnFailM{0};
static std::atomic<uint64_t> s_cnFailN{0};
static std::atomic<uint32_t> s_cnLoggedThisSec{0};
static auto s_cnLastSecLog = std::chrono::steady_clock::now();

static std::atomic<uint64_t> s_cqFastTaken{0};
static std::atomic<uint64_t> s_cqGeneric{0};
static std::atomic<uint64_t> s_cqChecked{0};
static std::atomic<uint64_t> s_cqFastOk{0};
static std::atomic<uint64_t> s_cqMismPx{0};
static std::atomic<uint32_t> s_cqMismLogged{0};
static auto s_cqLastSecLog = std::chrono::steady_clock::now();

static inline bool isSpriteFastEnabled() {
    static int s_val = -1;
    if (s_val == -1) {
        const char *env = std::getenv("PS2X_GS_SPRITE_FAST");
        s_val = (env && (env[0] == '0' || env[0] == 'f' || env[0] == 'F')) ? 0 : 1;
    }
    return s_val == 1;
}

static inline bool isSpriteVerifyEnabled() {
    static int s_val = -1;
    if (s_val == -1) {
        const char *env = std::getenv("PS2X_GS_SPRITE_VERIFY");
        s_val = (env && (env[0] == '1' || env[0] == 't' || env[0] == 'T')) ? 1 : 0;
    }
    return s_val == 1;
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        const uint32_t fpsm = ctx.frame.psm;
        const uint32_t fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        const uint32_t zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
        const uint32_t zpsm = ctx.zbuf.psm;

        const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
        const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2) & 0x3u);

        const int texU0 = static_cast<int>(v0.u >> 4);
        const int texV0 = static_cast<int>(v0.v >> 4);
        const int texU1 = static_cast<int>(v1.u >> 4);
        const int texV1 = static_cast<int>(v1.v >> 4);

        const bool isUvAligned = ((v0.u & 0xFu) == 0u) && ((v0.v & 0xFu) == 0u) &&
                                 ((v1.u & 0xFu) == 0u) && ((v1.v & 0xFu) == 0u);
        const bool isOneToOne = ((texU1 - texU0) == spanX) && ((texV1 - texV0) == spanY);
        const bool isUvInBounds = (texU0 >= 0) && (texU1 <= texW) && (texV0 >= 0) && (texV1 <= texH);
        const bool isWrapSupported = (wrapU <= 1u) && (wrapV <= 1u);

        const bool isFormatSupported = (tex.psm == GS_PSM_CT32) && (fpsm == GS_PSM_CT32);

        const bool isColorIdentity = (tex.tcc != 0u) &&
            ((tex.tfx == 1u) ||
             (tex.tfx == 0u && v1.r == 0x80u && v1.g == 0x80u && v1.b == 0x80u && v1.a == 0x80u));

        const bool alphaTestOff = (ctx.test & 0x1u) == 0u;
        const bool destAlphaTestOff = ((ctx.test >> 14) & 0x1u) == 0u;
        const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
        const bool depthPass = ((ctx.test & (1u << 16)) == 0u) || (ztestMethod == 1u);
        const bool writeDepth = !ctx.zbuf.zmask;

        const bool isWritePixelPlain = (!state.prim.abe) && (!state.prim.fge) &&
                                       (!state.dthe) && (ctx.frame.fbmsk == 0u) &&
                                       ((ctx.fba & 1ull) == 0ull) && alphaTestOff &&
                                       destAlphaTestOff && depthPass;

        const bool canFastPath = state.prim.fst && isUvAligned && isOneToOne && isUvInBounds &&
                                 isWrapSupported && isFormatSupported && isColorIdentity &&
                                 isWritePixelPlain;

        if (spanX * spanY >= 20000)
        {
            s_cnBigSprites.fetch_add(1u, std::memory_order_relaxed);

            uint32_t failmask = 0;
            // a: 1:1 mapping
            if (!isOneToOne) { failmask |= (1u << 0); s_cnFailA.fetch_add(1u, std::memory_order_relaxed); }
            // b: u0/v0 integer
            if (!isUvAligned) { failmask |= (1u << 1); s_cnFailB.fetch_add(1u, std::memory_order_relaxed); }
            // c: source rect inside texture
            if (!isUvInBounds) { failmask |= (1u << 2); s_cnFailC.fetch_add(1u, std::memory_order_relaxed); }
            // d: wrap mode 0/1
            if (!isWrapSupported) { failmask |= (1u << 3); s_cnFailD.fetch_add(1u, std::memory_order_relaxed); }
            // e: tex psm CT32
            if (tex.psm != GS_PSM_CT32) { failmask |= (1u << 4); s_cnFailE.fetch_add(1u, std::memory_order_relaxed); }
            // f: dest psm CT32
            if (fpsm != GS_PSM_CT32) { failmask |= (1u << 5); s_cnFailF.fetch_add(1u, std::memory_order_relaxed); }
            // g: combine identity (tfx/colour)
            if (!isColorIdentity) { failmask |= (1u << 6); s_cnFailG.fetch_add(1u, std::memory_order_relaxed); }
            // h: abe off
            if (state.prim.abe) { failmask |= (1u << 7); s_cnFailH.fetch_add(1u, std::memory_order_relaxed); }
            // i: ate off
            if (!alphaTestOff) { failmask |= (1u << 8); s_cnFailI.fetch_add(1u, std::memory_order_relaxed); }
            // j: depth test
            if (!depthPass) { failmask |= (1u << 9); s_cnFailJ.fetch_add(1u, std::memory_order_relaxed); }
            // k: z write
            // In CM, writeDepth is reproduced unconditionally, so depth write never fails fast path (always 0)
            // But if we check if z write is disabled or not, depth write is handled. Failbit k is 0.
            // l: fog off
            if (state.prim.fge) { failmask |= (1u << 11); s_cnFailL.fetch_add(1u, std::memory_order_relaxed); }
            // m: fbmsk
            if (ctx.frame.fbmsk != 0u) { failmask |= (1u << 12); s_cnFailM.fetch_add(1u, std::memory_order_relaxed); }
            // n: other conditions you added (!fst, destAlphaTest, fba, dthe)
            if (!state.prim.fst || !destAlphaTestOff || ((ctx.fba & 1ull) != 0ull) || state.dthe)
            {
                failmask |= (1u << 13);
                s_cnFailN.fetch_add(1u, std::memory_order_relaxed);
            }

            if (failmask == 0)
            {
                s_cnPass.fetch_add(1u, std::memory_order_relaxed);
            }

            auto nowCn = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(nowCn - s_cnLastSecLog).count() >= 1)
            {
                s_cnLastSecLog = nowCn;
                s_cnLoggedThisSec.store(0, std::memory_order_relaxed);
                probe_ae::bhLog("[probe] cn fail histogram big_sprites=%llu pass=%llu failbits{a=%llu b=%llu c=%llu d=%llu e=%llu f=%llu g=%llu h=%llu i=%llu j=%llu k=%llu l=%llu m=%llu n=%llu}\n",
                                (unsigned long long)s_cnBigSprites.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnPass.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailA.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailB.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailC.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailD.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailE.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailF.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailG.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailH.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailI.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailJ.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailK.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailL.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailM.load(std::memory_order_relaxed),
                                (unsigned long long)s_cnFailN.load(std::memory_order_relaxed));
            }

            uint32_t loggedCount = s_cnLoggedThisSec.fetch_add(1u, std::memory_order_relaxed);
            if (loggedCount < 3u)
            {
                uint32_t ate = ctx.test & 0x1u;
                uint32_t zte = (ctx.test >> 16) & 0x1u;
                uint32_t zmsk = ctx.zbuf.zmask ? 1u : 0u;
                probe_ae::bhLog("[probe] cn spr x=(%d,%d)-(%d,%d) span=%dx%d fst=%u uv0=(%u,%u) uv1=(%u,%u) texW=%d texH=%d tex.psm=%u tbp0=0x%x tbw=%u lin=%u tfx=%u tcc=%u col=%u,%u,%u,%u abe=%u ate=%u zte=%u zmsk=%u fbmsk=0x%x frame.psm=%u fbp=0x%x wrapU/V=%u/%u clamp=0x%llx failmask=0x%x\n",
                                x0, y0, x1, y1, spanX, spanY, state.prim.fst ? 1u : 0u,
                                (unsigned)v0.u, (unsigned)v0.v, (unsigned)v1.u, (unsigned)v1.v,
                                texW, texH, (unsigned)tex.psm, (unsigned)tex.tbp0, (unsigned)tex.tbw,
                                state.linearFilter ? 1u : 0u, (unsigned)tex.tfx, (unsigned)tex.tcc,
                                (unsigned)v1.r, (unsigned)v1.g, (unsigned)v1.b, (unsigned)v1.a,
                                state.prim.abe ? 1u : 0u, ate, zte, zmsk,
                                (unsigned)ctx.frame.fbmsk, (unsigned)fpsm, (unsigned)fbp,
                                (unsigned)wrapU, (unsigned)wrapV, (unsigned long long)ctx.clamp,
                                (unsigned)failmask);
            }
        }

        if (canFastPath && isSpriteFastEnabled() && !isSpriteVerifyEnabled())
        {
            s_cmFastTaken.fetch_add(1u, std::memory_order_relaxed);
            const uint32_t drawnPxCount = static_cast<uint32_t>((drawY1 - drawY0 + 1) * (drawX1 - drawX0 + 1));
            g_profPixels.fetch_add(drawnPxCount, std::memory_order_relaxed);

            for (int y = drawY0; y <= drawY1; ++y)
            {
                const int sy = texV0 + (y - unclippedY0);
                for (int x = drawX0; x <= drawX1; ++x)
                {
                    const int sx = texU0 + (x - unclippedX0);
                    const uint32_t texel = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sx, sy);
                    WriteVramUnlocked(fpsm, fbp, fbw, x, y, texel);
                    if (writeDepth)
                    {
                        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z1);
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_cmLastSecLog).count() >= 1)
            {
                s_cmLastSecLog = now;
                probe_ae::bhLog("[probe] cm sprite fast_taken=%llu generic=%llu\n",
                                (unsigned long long)s_cmFastTaken.load(std::memory_order_relaxed),
                                (unsigned long long)s_cmGeneric.load(std::memory_order_relaxed));
            }
            return;
        }

        // =========================================================================
        // Task CQ: Fast path for 1:1 CT32 bilinear copy with identity blend
        // =========================================================================
        bool canCqFast = false;
        float cqFx = 0.0f;
        float cqFy = 0.0f;
        std::vector<int> cqU0_tab;
        std::vector<int> cqU1_tab;
        const uint16_t minU = static_cast<uint16_t>(ctx.clamp >> 8) & 0x3FFu;
        const uint16_t maxU = static_cast<uint16_t>(ctx.clamp >> 18) & 0x3FFu;
        const uint16_t minV = static_cast<uint16_t>(ctx.clamp >> 28) & 0x3FFu;
        const uint16_t maxV = static_cast<uint16_t>(ctx.clamp >> 38) & 0x3FFu;

        if (state.prim.fst && state.linearFilter &&
            tex.psm == GS_PSM_CT32 && fpsm == GS_PSM_CT32 &&
            !state.prim.fge && !state.dthe &&
            (ctx.frame.fbmsk == 0u || ctx.frame.fbmsk == 0xFF000000u))
        {
            // 1:1 scale check
            if ((u1f - u0f) == spriteW && (v1f - v0f) == spriteH)
            {
                // Check combine identity: TFX modulate with vertex r=g=b=0x80, or DECAL
                const bool isModulateIdentity = (tex.tfx == 0u && v1.r == 0x80u && v1.g == 0x80u && v1.b == 0x80u);
                const bool isDecalIdentity = (tex.tfx == 1u);
                if (isModulateIdentity || isDecalIdentity)
                {
                    // Check blend identity: abe==0, or ALPHA reg gives result == Cs
                    bool blendIdentity = false;
                    if (!state.prim.abe)
                    {
                        blendIdentity = true;
                    }
                    else
                    {
                        uint64_t alphaReg = ctx.alpha;
                        uint8_t asel = alphaReg & 3;
                        uint8_t bsel = (alphaReg >> 2) & 3;
                        uint8_t csel = (alphaReg >> 4) & 3;
                        uint8_t dsel = (alphaReg >> 6) & 3;
                        uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);
                        // Blend formula: ((A - B) * C >> 7) + D
                        // D == 0 (Cs) and (A == B or (C == 2 (FIX) and FIX == 0))
                        if (dsel == 0 && (asel == bsel || (csel == 2 && fix == 0)))
                        {
                            blendIdentity = true;
                        }
                    }

                    if (blendIdentity)
                    {
                        // Check alpha test: ate == 0 or passesAlphaTest is always true
                        const bool ateOff = (ctx.test & 0x1u) == 0u;
                        const uint8_t atst = static_cast<uint8_t>((ctx.test >> 1) & 0x7u);
                        const bool atePasses = ateOff || (atst == 1u);

                        // Check destination alpha test: date off
                        const bool dateOff = ((ctx.test >> 14) & 0x1u) == 0u;

                        // Check depth test
                        if (atePasses && dateOff && depthPass)
                        {
                            // Precompute sample fraction from (drawX0, drawY0)
                            float tx0 = (static_cast<float>(drawX0 - unclippedX0) + 0.5f) / spriteW;
                            float ty0 = (static_cast<float>(drawY0 - unclippedY0) + 0.5f) / spriteH;
                            float firstUf = u0f + (u1f - u0f) * tx0;
                            float firstVf = v0f + (v1f - v0f) * ty0;
                            int fixedU = static_cast<int>((firstUf * 16.0f) + 0.5f);
                            int fixedV = static_cast<int>((firstVf * 16.0f) + 0.5f);
                            float sampleU0 = static_cast<float>(clampInt(fixedU, 0, 0xFFFF)) / 16.0f;
                            float sampleV0 = static_cast<float>(clampInt(fixedV, 0, 0xFFFF)) / 16.0f;
                            float subU = sampleU0 - 0.5f;
                            float subV = sampleV0 - 0.5f;
                            int iU0 = static_cast<int>(std::floor(subU));
                            int iV0 = static_cast<int>(std::floor(subV));
                            cqFx = subU - static_cast<float>(iU0);
                            cqFy = subV - static_cast<float>(iV0);

                            // Precompute columns for x in [drawX0, drawX1]
                            const int numCols = drawX1 - drawX0 + 1;
                            cqU0_tab.resize(numCols);
                            cqU1_tab.resize(numCols);
                            for (int x = drawX0; x <= drawX1; ++x)
                            {
                                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                                float u_f = u0f + (u1f - u0f) * tx;
                                int fU = static_cast<int>((u_f * 16.0f) + 0.5f);
                                float sU = static_cast<float>(clampInt(fU, 0, 0xFFFF)) / 16.0f;
                                int su0 = static_cast<int>(std::floor(sU - 0.5f));
                                int su1 = su0 + 1;
                                cqU0_tab[x - drawX0] = wrapTextureCoordinate(su0, texW, wrapU, minU, maxU);
                                cqU1_tab[x - drawX0] = wrapTextureCoordinate(su1, texW, wrapU, minU, maxU);
                            }
                            canCqFast = true;
                        }
                    }
                }
            }
        }

        if (canCqFast && isSpriteFastEnabled() && !isSpriteVerifyEnabled())
        {
            s_cqFastTaken.fetch_add(1u, std::memory_order_relaxed);
            const uint32_t drawnPxCount = static_cast<uint32_t>((drawY1 - drawY0 + 1) * (drawX1 - drawX0 + 1));
            g_profPixels.fetch_add(drawnPxCount, std::memory_order_relaxed);

            const bool maskAlpha = (ctx.frame.fbmsk == 0xFF000000u);
            const int numCols = drawX1 - drawX0 + 1;
            std::vector<uint32_t> row0(numCols);
            std::vector<uint32_t> row1(numCols);
            int cachedV0 = -999999;
            int cachedV1 = -999999;

            for (int y = drawY0; y <= drawY1; ++y)
            {
                float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
                float v_f = v0f + (v1f - v0f) * ty;
                int fV = static_cast<int>((v_f * 16.0f) + 0.5f);
                float sV = static_cast<float>(clampInt(fV, 0, 0xFFFF)) / 16.0f;
                int sv0 = static_cast<int>(std::floor(sV - 0.5f));
                int sv1 = sv0 + 1;
                int rowV0 = wrapTextureCoordinate(sv0, texH, wrapV, minV, maxV);
                int rowV1 = wrapTextureCoordinate(sv1, texH, wrapV, minV, maxV);

                if (rowV0 != cachedV0)
                {
                    for (int i = 0; i < numCols; ++i)
                    {
                        // Note: cqU0_tab and cqU1_tab can share texels across adjacent pixels,
                        // but reading directly into row buffers or per-column is fast.
                    }
                    cachedV0 = rowV0;
                }

                // If fy is exactly 0.0f, row1 is completely unused by lerpChannel
                const bool isFyZero = (cqFy == 0.0f);

                for (int x = drawX0; x <= drawX1; ++x)
                {
                    const int colIdx = x - drawX0;
                    const int tapU0 = cqU0_tab[colIdx];
                    const int tapU1 = cqU1_tab[colIdx];

                    const uint32_t c00 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU0, rowV0);
                    const uint32_t c10 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU1, rowV0);

                    uint8_t r_out, g_out, b_out;
                    if (isFyZero)
                    {
                        const float topR = static_cast<float>(static_cast<uint8_t>(c00 & 0xFFu)) +
                                           (static_cast<float>(static_cast<uint8_t>(c10 & 0xFFu)) - static_cast<float>(static_cast<uint8_t>(c00 & 0xFFu))) * cqFx;
                        const float topG = static_cast<float>(static_cast<uint8_t>((c00 >> 8) & 0xFFu)) +
                                           (static_cast<float>(static_cast<uint8_t>((c10 >> 8) & 0xFFu)) - static_cast<float>(static_cast<uint8_t>((c00 >> 8) & 0xFFu))) * cqFx;
                        const float topB = static_cast<float>(static_cast<uint8_t>((c00 >> 16) & 0xFFu)) +
                                           (static_cast<float>(static_cast<uint8_t>((c10 >> 16) & 0xFFu)) - static_cast<float>(static_cast<uint8_t>((c00 >> 16) & 0xFFu))) * cqFx;
                        r_out = clampU8(static_cast<int>(std::lround(topR)));
                        g_out = clampU8(static_cast<int>(std::lround(topG)));
                        b_out = clampU8(static_cast<int>(std::lround(topB)));
                    }
                    else
                    {
                        const uint32_t c01 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU0, rowV1);
                        const uint32_t c11 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU1, rowV1);
                        r_out = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu), static_cast<uint8_t>(c10 & 0xFFu),
                                            static_cast<uint8_t>(c01 & 0xFFu), static_cast<uint8_t>(c11 & 0xFFu), cqFx, cqFy);
                        g_out = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu), static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 8) & 0xFFu), static_cast<uint8_t>((c11 >> 8) & 0xFFu), cqFx, cqFy);
                        b_out = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu), static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 16) & 0xFFu), static_cast<uint8_t>((c11 >> 16) & 0xFFu), cqFx, cqFy);
                    }

                    uint32_t outPixel = static_cast<uint32_t>(r_out) |
                                        (static_cast<uint32_t>(g_out) << 8) |
                                        (static_cast<uint32_t>(b_out) << 16);

                    if (maskAlpha)
                    {
                        const uint32_t dstPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
                        outPixel |= (dstPixel & 0xFF000000u);
                    }
                    else
                    {
                        outPixel |= 0xFF000000u;
                    }

                    WriteVramUnlocked(fpsm, fbp, fbw, x, y, outPixel);
                    if (writeDepth)
                    {
                        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z1);
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_cqLastSecLog).count() >= 1)
            {
                s_cqLastSecLog = now;
                probe_ae::bhLog("[probe] cq sprite fast_taken=%llu generic=%llu\n",
                                (unsigned long long)s_cqFastTaken.load(std::memory_order_relaxed),
                                (unsigned long long)s_cqGeneric.load(std::memory_order_relaxed));
            }
            return;
        }

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
            }
        }

        if (canFastPath && isSpriteVerifyEnabled())
        {
            s_cmChecked.fetch_add(1u, std::memory_order_relaxed);
            bool spriteMatch = true;
            for (int y = drawY0; y <= drawY1; ++y)
            {
                const int sy = texV0 + (y - unclippedY0);
                for (int x = drawX0; x <= drawX1; ++x)
                {
                    const int sx = texU0 + (x - unclippedX0);
                    const uint32_t expected = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sx, sy);
                    const uint32_t got = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
                    if (expected != got)
                    {
                        spriteMatch = false;
                        s_cmMismPx.fetch_add(1u, std::memory_order_relaxed);
                        uint32_t logged = s_cmMismLogged.fetch_add(1u, std::memory_order_relaxed);
                        if (logged < 5u)
                        {
                            probe_ae::bhLog("[probe] cm sprite mism x=%d y=%d exp=0x%08X got=0x%08X\n", x, y, expected, got);
                        }
                    }
                }
            }
            if (spriteMatch)
            {
                s_cmFastOk.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        if (canCqFast && isSpriteVerifyEnabled())
        {
            s_cqChecked.fetch_add(1u, std::memory_order_relaxed);
            bool spriteMatch = true;
            const bool maskAlpha = (ctx.frame.fbmsk == 0xFF000000u);
            const bool isFyZero = (cqFy == 0.0f);

            for (int y = drawY0; y <= drawY1; ++y)
            {
                float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
                float v_f = v0f + (v1f - v0f) * ty;
                int fV = static_cast<int>((v_f * 16.0f) + 0.5f);
                float sV = static_cast<float>(clampInt(fV, 0, 0xFFFF)) / 16.0f;
                int sv0 = static_cast<int>(std::floor(sV - 0.5f));
                int sv1 = sv0 + 1;
                int rowV0 = wrapTextureCoordinate(sv0, texH, wrapV, minV, maxV);
                int rowV1 = wrapTextureCoordinate(sv1, texH, wrapV, minV, maxV);

                for (int x = drawX0; x <= drawX1; ++x)
                {
                    const int colIdx = x - drawX0;
                    const int tapU0 = cqU0_tab[colIdx];
                    const int tapU1 = cqU1_tab[colIdx];

                    const uint32_t c00 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU0, rowV0);
                    const uint32_t c10 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU1, rowV0);

                    uint8_t r_out, g_out, b_out;
                    if (isFyZero)
                    {
                        const float topR = static_cast<float>(static_cast<uint8_t>(c00 & 0xFFu)) +
                                           (static_cast<float>(static_cast<uint8_t>(c10 & 0xFFu)) - static_cast<float>(static_cast<uint8_t>(c00 & 0xFFu))) * cqFx;
                        const float topG = static_cast<float>(static_cast<uint8_t>((c00 >> 8) & 0xFFu)) +
                                           (static_cast<float>(static_cast<uint8_t>((c10 >> 8) & 0xFFu)) - static_cast<float>(static_cast<uint8_t>((c00 >> 8) & 0xFFu))) * cqFx;
                        const float topB = static_cast<float>(static_cast<uint8_t>((c00 >> 16) & 0xFFu)) +
                                           (static_cast<float>(static_cast<uint8_t>((c10 >> 16) & 0xFFu)) - static_cast<float>(static_cast<uint8_t>((c00 >> 16) & 0xFFu))) * cqFx;
                        r_out = clampU8(static_cast<int>(std::lround(topR)));
                        g_out = clampU8(static_cast<int>(std::lround(topG)));
                        b_out = clampU8(static_cast<int>(std::lround(topB)));
                    }
                    else
                    {
                        const uint32_t c01 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU0, rowV1);
                        const uint32_t c11 = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, tapU1, rowV1);
                        r_out = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu), static_cast<uint8_t>(c10 & 0xFFu),
                                            static_cast<uint8_t>(c01 & 0xFFu), static_cast<uint8_t>(c11 & 0xFFu), cqFx, cqFy);
                        g_out = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu), static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 8) & 0xFFu), static_cast<uint8_t>((c11 >> 8) & 0xFFu), cqFx, cqFy);
                        b_out = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu), static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                            static_cast<uint8_t>((c01 >> 16) & 0xFFu), static_cast<uint8_t>((c11 >> 16) & 0xFFu), cqFx, cqFy);
                    }

                    const uint32_t got = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
                    bool mismatch = false;
                    if (maskAlpha)
                    {
                        // Alpha masked: compare RGB only
                        if ((static_cast<uint32_t>(r_out) != (got & 0xFFu)) ||
                            (static_cast<uint32_t>(g_out) != ((got >> 8) & 0xFFu)) ||
                            (static_cast<uint32_t>(b_out) != ((got >> 16) & 0xFFu)))
                        {
                            mismatch = true;
                        }
                    }
                    else
                    {
                        uint32_t expected = static_cast<uint32_t>(r_out) |
                                            (static_cast<uint32_t>(g_out) << 8) |
                                            (static_cast<uint32_t>(b_out) << 16) | 0xFF000000u;
                        if (expected != got)
                        {
                            mismatch = true;
                        }
                    }

                    if (mismatch)
                    {
                        spriteMatch = false;
                        s_cqMismPx.fetch_add(1u, std::memory_order_relaxed);
                        uint32_t logged = s_cqMismLogged.fetch_add(1u, std::memory_order_relaxed);
                        if (logged < 5u)
                        {
                            uint32_t expRgb = static_cast<uint32_t>(r_out) |
                                              (static_cast<uint32_t>(g_out) << 8) |
                                              (static_cast<uint32_t>(b_out) << 16);
                            probe_ae::bhLog("[probe] cq sprite mism x=%d y=%d exp=0x%08X got=0x%08X\n", x, y, expRgb, got);
                        }
                    }
                }
            }
            if (spriteMatch)
            {
                s_cqFastOk.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        s_cmGeneric.fetch_add(1u, std::memory_order_relaxed);
        s_cqGeneric.fetch_add(1u, std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - s_cmLastSecLog).count() >= 1)
        {
            s_cmLastSecLog = now;
            if (isSpriteVerifyEnabled())
            {
                probe_ae::bhLog("[probe] cm sprite fast_ok=%llu checked=%llu mism_px=%llu\n",
                                (unsigned long long)s_cmFastOk.load(std::memory_order_relaxed),
                                (unsigned long long)s_cmChecked.load(std::memory_order_relaxed),
                                (unsigned long long)s_cmMismPx.load(std::memory_order_relaxed));
                probe_ae::bhLog("[probe] cq sprite fast_ok=%llu checked=%llu mism_px=%llu\n",
                                (unsigned long long)s_cqFastOk.load(std::memory_order_relaxed),
                                (unsigned long long)s_cqChecked.load(std::memory_order_relaxed),
                                (unsigned long long)s_cqMismPx.load(std::memory_order_relaxed));
            }
            probe_ae::bhLog("[probe] cm sprite fast_taken=%llu generic=%llu\n",
                            (unsigned long long)s_cmFastTaken.load(std::memory_order_relaxed),
                            (unsigned long long)s_cmGeneric.load(std::memory_order_relaxed));
            probe_ae::bhLog("[probe] cq sprite fast_taken=%llu generic=%llu\n",
                            (unsigned long long)s_cqFastTaken.load(std::memory_order_relaxed),
                            (unsigned long long)s_cqGeneric.load(std::memory_order_relaxed));
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    for (int y = minY; y <= maxY; ++y)
    {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
            WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

static void flushCurrentUploadDump()
{
    if (!s_inUpload || s_currentUploadPayload.empty())
        return;
    s_inUpload = false;
    if (s_uploadSeq >= 64u)
        return;

    std::filesystem::create_directories("logs/gs");
    uint32_t seq = ++s_uploadSeq;
    uint32_t crc = calculateCrc32(s_currentUploadPayload.data(), s_currentUploadPayload.size());
    char fn[256];
    std::snprintf(fn, sizeof(fn), "logs/gs/upload_%03u_dbp%u_psm0x%02x_%ux%u.bin",
                  seq, s_currentUploadCommand.bitbltbuf.dbp, static_cast<unsigned>(s_currentUploadCommand.bitbltbuf.dpsm),
                  s_currentUploadCommand.trxreg.rrw, s_currentUploadCommand.trxreg.rrh);

    FILE *fp = std::fopen(fn, "wb");
    if (fp)
    {
        std::fwrite(s_currentUploadPayload.data(), 1, s_currentUploadPayload.size(), fp);
        std::fclose(fp);
    }
    std::fprintf(stderr, "[GS:UPLOAD] seq=%u BITBLTBUF(dbp=%u, dbw=%u, dpsm=0x%02x) TRXPOS(dsax=%u, dsay=%u) TRXREG(rrw=%u, rrh=%u) TRXDIR=%u bytes=%zu crc32=0x%08X\n",
                 seq, s_currentUploadCommand.bitbltbuf.dbp, s_currentUploadCommand.bitbltbuf.dbw,
                 static_cast<unsigned>(s_currentUploadCommand.bitbltbuf.dpsm),
                 s_currentUploadCommand.trxpos.dsax, s_currentUploadCommand.trxpos.dsay,
                 s_currentUploadCommand.trxreg.rrw, s_currentUploadCommand.trxreg.rrh,
                 s_currentUploadCommand.direction, s_currentUploadPayload.size(), crc);
    std::fflush(stderr);
    s_currentUploadPayload.clear();
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (s_uploadDump == -1)
    {
        const char *env = std::getenv("PS2X_GS_UPLOAD_DUMP");
        s_uploadDump = (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0)) ? 1 : 0;
    }
    if (s_uploadDump == 1)
    {
        flushCurrentUploadDump();
        if (command.direction == 0u)
        {
            s_inUpload = true;
            s_currentUploadCommand = command;
            s_currentUploadPayload.clear();
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (s_uploadDump == 1 && s_inUpload && data && sizeBytes > 0u)
    {
        if (s_currentUploadPayload.size() + sizeBytes <= 8u * 1024u * 1024u)
        {
            s_currentUploadPayload.insert(s_currentUploadPayload.end(), data, data + sizeBytes);
        }
    }
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const auto t0 = std::chrono::steady_clock::now();
    struct XferScope {
        std::chrono::steady_clock::time_point start;
        ~XferScope() {
            auto end = std::chrono::steady_clock::now();
            g_profXferUs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()), std::memory_order_relaxed);
        }
    } xferScope{t0};

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    if (dbp == 10752)
    {
        static bool s_dumped10752 = false;
        if (!s_dumped10752)
        {
            s_dumped10752 = true;
            std::filesystem::create_directories("logs/gs");
            FILE *f = std::fopen("logs/gs/upload_fixed_10752.bin", "wb");
            if (f) { std::fwrite(data, 1, sizeBytes, f); std::fclose(f); }
            std::fprintf(stderr, "[GS:UPLOAD_IMAGE] Dumped fixed upload to logs/gs/upload_fixed_10752.bin (size=%u)\n", sizeBytes);
            std::fflush(stderr);
        }
    }
    if (dbp == 10784)
    {
        static bool s_dumped10784 = false;
        if (!s_dumped10784)
        {
            s_dumped10784 = true;
            std::filesystem::create_directories("logs/gs");
            FILE *f = std::fopen("logs/gs/upload_fixed_10784.bin", "wb");
            if (f) { std::fwrite(data, 1, sizeBytes, f); std::fclose(f); }
            std::fprintf(stderr, "[GS:UPLOAD_IMAGE] Dumped fixed upload to logs/gs/upload_fixed_10784.bin (size=%u)\n", sizeBytes);
            std::fflush(stderr);
        }
    }
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    auto checkUploadFinished = [&]()
    {
        if (s_uploadDump == 1 && s_inUpload && m_transferState.direction == 3u)
        {
            flushCurrentUploadDump();
        }
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
    checkUploadFinished();
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    PresentationFrame frame = snapshotBackend.PresentFromLocalMemory(request);
    if (probe_ae::isEnabled())
    {
        static uint32_t s_frameCounter = 0;
        GSFrameReg dispFrame = decodeDisplayFrame(request.dispfb1 ? request.dispfb1 : request.dispfb2);
        probe_ae::onAyCheckWhiteStrip(++s_frameCounter, dispFrame.fbp, dispFrame.fbw, dispFrame.psm, frame.pixels.data(), kHostFrameWidth, frame.height);
    }
    return frame;
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (s_presentFallback == -1)
        {
            const char *env = std::getenv("PS2X_GS_PRESENT_FALLBACK");
            s_presentFallback = (env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0)) ? 0 : 1;
        }
        if (s_presentFallback != 0 && !usedPreferred && countNonBlackPixels(pixels, width, height) <= 256u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}

// Global VRAM dump function for Step 1.b
void ps2xDumpVramAndState(const char *triggerName, uint64_t tick, void *rtVoid)
{
    if (!s_cpuBackendInstance)
        return;

    std::filesystem::create_directories("logs/gs");
    char binFn[256], txtFn[256];
    std::snprintf(binFn, sizeof(binFn), "logs/gs/vram_%llu.bin", static_cast<unsigned long long>(tick));
    std::snprintf(txtFn, sizeof(txtFn), "logs/gs/vram_%llu.txt", static_cast<unsigned long long>(tick));

    std::vector<uint8_t> vramSnap;
    s_cpuBackendInstance->SnapshotVram(vramSnap);

    if (!vramSnap.empty())
    {
        FILE *fp = std::fopen(binFn, "wb");
        if (fp)
        {
            std::fwrite(vramSnap.data(), 1, vramSnap.size(), fp);
            std::fclose(fp);
        }
    }

    FILE *tfp = std::fopen(txtFn, "w");
    if (tfp)
    {
        std::fprintf(tfp, "=== GS VRAM & State Dump (Tick %llu, Trigger: %s) ===\n",
                     static_cast<unsigned long long>(tick), triggerName ? triggerName : "unknown");

        // Registers from GSCpuBackend
        std::fprintf(tfp, "m_clutCbp[0] = %u\n", s_savedClutCbp[0]);
        std::fprintf(tfp, "m_clutCbp[1] = %u\n", s_savedClutCbp[1]);
        std::fprintf(tfp, "last_cld = %u\n\n", static_cast<unsigned>(s_lastSeenCld));

        // CLUT 256 entries
        std::fprintf(tfp, "=== CLUT (256 entries) ===\n");
        for (uint32_t i = 0; i < 256u; ++i)
        {
            uint32_t val32 = static_cast<uint32_t>(s_savedClut[i]) |
                             (static_cast<uint32_t>(s_savedClut[i + 256u]) << 16u);
            uint8_t r = static_cast<uint8_t>(val32 & 0xFFu);
            uint8_t g = static_cast<uint8_t>((val32 >> 8u) & 0xFFu);
            uint8_t b = static_cast<uint8_t>((val32 >> 16u) & 0xFFu);
            uint8_t a = static_cast<uint8_t>((val32 >> 24u) & 0xFFu);
            std::fprintf(tfp, "[%03u] R=%02x G=%02x B=%02x A=%02x (0x%08X)\n", i, r, g, b, a, val32);
        }

        std::fclose(tfp);
    }

    std::fprintf(stderr, "[GS:VRAM_DUMP] Triggered by %s at tick %llu -> %s and %s\n",
                 triggerName ? triggerName : "unknown", static_cast<unsigned long long>(tick), binFn, txtFn);
    std::fflush(stderr);
}
