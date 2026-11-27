#include "Common.h"
#include "IPU.h"
#include "runtime/ee_scheduler.h"
#include <cstdlib> // PROBE-IP1
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#if !defined(PS2X_HAS_FFMPEG)
#define PS2X_HAS_FFMPEG 1
#endif

#if PS2X_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}
#endif

extern void ps2xSafeLog(const char *fmt, ...);
extern void ipuProbeLogCall(uint32_t ra, const char *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t ret); // PROBE-IP1

namespace
{
    constexpr uint32_t REG_IPU_CTRL = 0x10002010u;
    constexpr uint32_t REG_IPU_CMD = 0x10002000u;

    inline bool ipuProbeOn() // PROBE-IP1
    { // PROBE-IP1
        static const bool s_on = []() { const char *e = std::getenv("PS2X_IPU_LOG"); return e && e[0] == '1'; }(); // PROBE-IP1
        return s_on; // PROBE-IP1
    } // PROBE-IP1

    struct IpuCallLogger // PROBE-IP1
    { // PROBE-IP1
        const char *name; // PROBE-IP1
        R5900Context *ctx; // PROBE-IP1
        uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, ra = 0; // PROBE-IP1
        IpuCallLogger(const char *n, R5900Context *c) : name(n), ctx(c) // PROBE-IP1
        { // PROBE-IP1
            if (ipuProbeOn() && ctx) // PROBE-IP1
            { // PROBE-IP1
                a0 = getRegU32(ctx, 4); // PROBE-IP1
                a1 = getRegU32(ctx, 5); // PROBE-IP1
                a2 = getRegU32(ctx, 6); // PROBE-IP1
                a3 = getRegU32(ctx, 7); // PROBE-IP1
                ra = getRegU32(ctx, 31); // PROBE-IP1
            } // PROBE-IP1
        } // PROBE-IP1
        ~IpuCallLogger() // PROBE-IP1
        { // PROBE-IP1
            if (ipuProbeOn()) // PROBE-IP1
            { // PROBE-IP1
                const uint32_t ret = ctx ? getRegU32(ctx, 2) : 0u; // PROBE-IP1
                ipuProbeLogCall(ra, name, a0, a1, a2, a3, ret); // PROBE-IP1
            } // PROBE-IP1
        } // PROBE-IP1
    }; // PROBE-IP1

    void completeIpuInit(PS2Runtime *runtime)
    {
        if (!runtime)
            return;
        PS2Memory &mem = runtime->memory();
        mem.write32(REG_IPU_CTRL, 0x40000000u);
        mem.write32(REG_IPU_CMD, 0u);
    }
}

namespace ps2_stubs
{
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuInit", ctx); // PROBE-IP1
        (void)rdram;
        if (!runtime)
            return;

        if (!runtime->memory().getRDRAM())
        {
            if (!runtime->memory().initialize())
            {
                setReturnS32(ctx, -1);
                return;
            }
        }

        if (!runtime->syncCoreSubsystems())
        {
            setReturnS32(ctx, -1);
            return;
        }

        completeIpuInit(runtime);
        setReturnS32(ctx, 0);
    }

    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuRestartDMA", ctx); // PROBE-IP1
        setReturnS32(ctx, 0);
    }

    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuStopDMA", ctx); // PROBE-IP1
        setReturnS32(ctx, 0);
    }

    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IpuCallLogger ipuLogger("sceIpuSync", ctx); // PROBE-IP1
        setReturnS32(ctx, 0);
    }
}

namespace
{
    struct BitWriter
    {
        std::vector<uint8_t> &buf;
        uint32_t bitBuf = 0;
        int bitCount = 0;

        explicit BitWriter(std::vector<uint8_t> &b) : buf(b) {}

        void putBits(uint32_t val, int n)
        {
            while (n > 0)
            {
                int room = 32 - bitCount;
                if (n <= room)
                {
                    bitBuf = (bitBuf << n) | (val & ((1ULL << n) - 1ULL));
                    bitCount += n;
                    n = 0;
                }
                else
                {
                    int take = room;
                    uint32_t top = (val >> (n - take)) & ((1ULL << take) - 1ULL);
                    bitBuf = (bitBuf << take) | top;
                    bitCount = 32;
                    flush32();
                    n -= take;
                }
                if (bitCount >= 8)
                {
                    flushBytes();
                }
            }
        }

        void flush32()
        {
            buf.push_back(static_cast<uint8_t>((bitBuf >> 24) & 0xFF));
            buf.push_back(static_cast<uint8_t>((bitBuf >> 16) & 0xFF));
            buf.push_back(static_cast<uint8_t>((bitBuf >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(bitBuf & 0xFF));
            bitBuf = 0;
            bitCount = 0;
        }

        void flushBytes()
        {
            while (bitCount >= 8)
            {
                int shift = bitCount - 8;
                uint8_t byte = static_cast<uint8_t>((bitBuf >> shift) & 0xFF);
                buf.push_back(byte);
                bitCount -= 8;
                bitBuf &= (1ULL << bitCount) - 1ULL;
            }
        }

        void putStartCode(uint32_t code)
        {
            align();
            buf.push_back(static_cast<uint8_t>((code >> 24) & 0xFF));
            buf.push_back(static_cast<uint8_t>((code >> 16) & 0xFF));
            buf.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(code & 0xFF));
        }

        void align()
        {
            if (bitCount > 0)
            {
                int pad = 8 - (bitCount % 8);
                if (pad != 8)
                {
                    putBits(0, pad);
                }
            }
        }
    };

    static inline uint8_t clampU8(int v)
    {
        return static_cast<uint8_t>(std::clamp(v, 0, 255));
    }
}

bool ps2xIpuIdecJob(uint8_t *rdram, uint32_t jobAddr)
{
    const char *envDecode = std::getenv("PS2X_IPU_DECODE");
    if (envDecode && envDecode[0] == '0')
    {
        return false;
    }

#if !PS2X_HAS_FFMPEG
    (void)rdram;
    (void)jobAddr;
    return false;
#else
    if (!rdram)
    {
        return false;
    }

    const uint32_t maskedJobAddr = jobAddr & 0x01FFFFFFu;
    if (maskedJobAddr + 16 > 32u * 1024u * 1024u)
    {
        return false;
    }

    const uint8_t *jobPtr = rdram + maskedJobAddr;
    const uint8_t cmd = jobPtr[2];
    const uint8_t ctl = jobPtr[3];
    uint16_t inQw = 0;
    uint16_t outQw = 0;
    std::memcpy(&inQw, jobPtr + 4, sizeof(inQw));
    std::memcpy(&outQw, jobPtr + 6, sizeof(outQw));

    uint32_t rawSrc = 0;
    uint32_t rawDst = 0;
    std::memcpy(&rawSrc, jobPtr + 8, sizeof(rawSrc));
    std::memcpy(&rawDst, jobPtr + 12, sizeof(rawDst));

    auto logResult = [&](bool ok) {
        static int s_loggedJobs = 0;
        const char *logEnv = std::getenv("PS2X_IPU_LOG");
        const bool alwaysLog = (logEnv && logEnv[0] == '1');
        if (s_loggedJobs < 8 || alwaysLog)
        {
            ++s_loggedJobs;
            const uint32_t N = outQw / 64u;
            char src8Hex[17] = {0};
            char out16Hex[33] = {0};

            const uint32_t srcMasked = rawSrc & 0x01FFFFFFu;
            const uint32_t inBytes = static_cast<uint32_t>(inQw) * 16u;
            if ((rawSrc & 0x80000000u) == 0 && (srcMasked + inBytes <= 32u * 1024u * 1024u))
            {
                const uint8_t *s = rdram + srcMasked;
                for (int i = 0; i < 8; ++i)
                {
                    std::snprintf(src8Hex + i * 2, 3, "%02x", s[i]);
                }
            }
            else
            {
                std::snprintf(src8Hex, sizeof(src8Hex), "????????????????");
            }

            const uint32_t dstMasked = rawDst & 0x01FFFFFFu;
            const uint32_t outBytes = static_cast<uint32_t>(N) * 1024u;
            if ((rawDst & 0x80000000u) == 0 && (dstMasked + outBytes <= 32u * 1024u * 1024u))
            {
                const uint8_t *d = rdram + dstMasked;
                for (int i = 0; i < 16; ++i)
                {
                    std::snprintf(out16Hex + i * 2, 3, "%02x", d[i]);
                }
            }
            else
            {
                std::snprintf(out16Hex, sizeof(out16Hex), "????????????????????????????????");
            }

            ps2xSafeLog("[ipu] job=0x%08x src=0x%08x dst=0x%08x n=%u ok=%d src8=%s out16=%s\n",
                        jobAddr, rawSrc, rawDst, N, ok ? 1 : 0, src8Hex, out16Hex);
        }
    };

    if ((cmd & 0xF0u) != 0x10u)
    {
        logResult(false);
        return false;
    }

    const uint32_t N = outQw / 64u;
    if (N < 1u || N > 256u)
    {
        logResult(false);
        return false;
    }

    if ((rawSrc & 0x80000000u) != 0 || (rawDst & 0x80000000u) != 0)
    {
        logResult(false);
        return false;
    }

    const uint32_t srcAddr = rawSrc & 0x01FFFFFFu;
    const uint32_t dstAddr = rawDst & 0x01FFFFFFu;
    const uint32_t inBytes = static_cast<uint32_t>(inQw) * 16u;
    const uint32_t outBytes = N * 1024u;

    if (static_cast<uint64_t>(srcAddr) + inBytes > 32ULL * 1024ULL * 1024ULL ||
        static_cast<uint64_t>(dstAddr) + outBytes > 32ULL * 1024ULL * 1024ULL)
    {
        logResult(false);
        return false;
    }

    const uint32_t IDP = ctl & 3u;
    const uint32_t AS  = (ctl >> 4u) & 1u;
    const uint32_t IVF = (ctl >> 5u) & 1u;
    const uint32_t QST = (ctl >> 6u) & 1u;

    uint32_t width = N * 16u;
    if (width >= 4096u)
    {
        width = width - 1u;
    }
    const uint32_t height = 16u;

    std::vector<uint8_t> es;
    es.reserve(inBytes + 256);
    BitWriter bw(es);

    // 000001B3 (Sequence Header)
    bw.putStartCode(0x000001B3u);
    bw.putBits(width & 0xFFFu, 12);
    bw.putBits(height & 0xFFFu, 12);
    bw.putBits(1u, 4);       // aspect_ratio_information = 1
    bw.putBits(4u, 4);       // frame_rate_code = 4
    bw.putBits(0x3FFFFu, 18);// bit_rate_value = 0x3FFFF
    bw.putBits(1u, 1);       // marker_bit = 1
    bw.putBits(112u, 10);    // vbv_buffer_size_value = 112
    bw.putBits(0u, 1);       // constrained_parameters_flag = 0
    bw.putBits(1u, 1);       // load_intra_quantiser_matrix = 1
    const uint8_t *matrixSrc = rdram + 0x0062EBB0u;
    for (int i = 0; i < 64; ++i)
    {
        bw.putBits(matrixSrc[i], 8);
    }
    bw.putBits(0u, 1);       // load_non_intra_quantiser_matrix = 0

    // 000001B5 (Sequence Extension)
    bw.putStartCode(0x000001B5u);
    bw.putBits(1u, 4);       // extension_start_code_identifier = 1
    bw.putBits(0x44u, 8);    // profile_and_level_indication = 0x44
    bw.putBits(1u, 1);       // progressive_sequence = 1
    bw.putBits(1u, 2);       // chroma_format = 1 (4:2:0)
    bw.putBits((width >> 12u) & 3u, 2); // horizontal_size_extension = width >> 12
    bw.putBits(0u, 2);       // vertical_size_extension = 0
    bw.putBits(0u, 12);      // bit_rate_extension = 0
    bw.putBits(1u, 1);       // marker_bit = 1
    bw.putBits(0u, 8);       // vbv_buffer_size_extension = 0
    bw.putBits(1u, 1);       // low_delay = 1
    bw.putBits(0u, 2);       // frame_rate_extension_n = 0
    bw.putBits(0u, 5);       // frame_rate_extension_d = 0

    // 00000100 (Picture Header)
    bw.putStartCode(0x00000100u);
    bw.putBits(0u, 10);      // temporal_reference = 0
    bw.putBits(1u, 3);       // picture_coding_type = 1 (I-frame)
    bw.putBits(0xFFFFu, 16); // vbv_delay = 0xFFFF
    bw.putBits(0u, 1);       // full_pel_forward_vector = 0 (and no extra picture info)

    // 000001B5 (Picture Coding Extension)
    bw.putStartCode(0x000001B5u);
    bw.putBits(8u, 4);       // extension_start_code_identifier = 8
    bw.putBits(0xFu, 4);     // f_code[0][0] = 0xF
    bw.putBits(0xFu, 4);     // f_code[0][1] = 0xF
    bw.putBits(0xFu, 4);     // f_code[1][0] = 0xF
    bw.putBits(0xFu, 4);     // f_code[1][1] = 0xF
    bw.putBits(IDP & 3u, 2); // intra_dc_precision = IDP
    bw.putBits(3u, 2);       // picture_structure = 3 (frame picture)
    bw.putBits(1u, 1);       // top_field_first = 1
    bw.putBits(1u, 1);       // frame_pred_frame_dct = 1
    bw.putBits(0u, 1);       // concealment_motion_vectors = 0
    bw.putBits(QST & 1u, 1); // q_scale_type = QST
    bw.putBits(IVF & 1u, 1); // intra_vlc_format = IVF
    bw.putBits(AS & 1u, 1);  // alternate_scan = AS
    bw.putBits(0u, 1);       // repeat_first_field = 0
    bw.putBits(1u, 1);       // chroma_420_type = 1
    bw.putBits(1u, 1);       // progressive_frame = 1
    bw.putBits(0u, 1);       // composite_display_flag = 0

    // 00000101 (Slice Header)
    bw.putStartCode(0x00000101u);
    bw.putBits(1u, 5);       // quantiser_scale_code = 1
    bw.putBits(0u, 1);       // slice_extension_flag = 0
    bw.putBits(1u, 1);       // macroblock_address_increment = 1 ('1')

    const uint8_t *srcData = rdram + srcAddr;
    for (uint32_t i = 0; i < inBytes; ++i)
    {
        bw.putBits(srcData[i], 8);
    }
    bw.align();

    // 000001B7 (Sequence End Code)
    bw.putStartCode(0x000001B7u);

    static std::mutex s_codecMutex;
    std::lock_guard<std::mutex> lock(s_codecMutex);

    static AVCodecContext *s_codecCtx = nullptr;
    static AVFrame *s_frame = nullptr;
    static AVPacket *s_packet = nullptr;
    static bool s_codecInitAttempted = false;

    if (!s_codecInitAttempted)
    {
        s_codecInitAttempted = true;
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
        if (codec)
        {
            s_codecCtx = avcodec_alloc_context3(codec);
            s_frame = av_frame_alloc();
            s_packet = av_packet_alloc();
            if (s_codecCtx && s_frame && s_packet)
            {
                s_codecCtx->thread_count = 1;
                s_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                if (avcodec_open2(s_codecCtx, codec, nullptr) < 0)
                {
                    avcodec_free_context(&s_codecCtx);
                    av_frame_free(&s_frame);
                    av_packet_free(&s_packet);
                }
            }
            else
            {
                if (s_codecCtx) avcodec_free_context(&s_codecCtx);
                if (s_frame) av_frame_free(&s_frame);
                if (s_packet) av_packet_free(&s_packet);
            }
        }
    }

    if (!s_codecCtx || !s_frame || !s_packet)
    {
        logResult(false);
        return false;
    }

    size_t esSize = es.size();
    size_t paddedSize = esSize + AV_INPUT_BUFFER_PADDING_SIZE;
    std::vector<uint8_t> paddedBuf(paddedSize, 0);
    std::memcpy(paddedBuf.data(), es.data(), esSize);

    s_packet->data = paddedBuf.data();
    s_packet->size = static_cast<int>(esSize);

    int ret = avcodec_send_packet(s_codecCtx, s_packet);
    if (ret < 0)
    {
        avcodec_flush_buffers(s_codecCtx);
        logResult(false);
        return false;
    }

    ret = avcodec_send_packet(s_codecCtx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF)
    {
        avcodec_flush_buffers(s_codecCtx);
        logResult(false);
        return false;
    }

    ret = avcodec_receive_frame(s_codecCtx, s_frame);
    if (ret < 0)
    {
        avcodec_flush_buffers(s_codecCtx);
        logResult(false);
        return false;
    }

    if (s_frame->height < 16 || s_frame->format != AV_PIX_FMT_YUV420P)
    {
        av_frame_unref(s_frame);
        avcodec_flush_buffers(s_codecCtx);
        logResult(false);
        return false;
    }

    const uint8_t *yPlane = s_frame->data[0];
    const uint8_t *uPlane = s_frame->data[1];
    const uint8_t *vPlane = s_frame->data[2];
    const int yStride = s_frame->linesize[0];
    const int uStride = s_frame->linesize[1];
    const int vStride = s_frame->linesize[2];

    uint8_t *dstBase = rdram + dstAddr;

    for (uint32_t mb = 0; mb < N; ++mb)
    {
        uint8_t *mbDst = dstBase + mb * 1024u;
        const int mbX = static_cast<int>(mb * 16u);

        for (int row = 0; row < 16; ++row)
        {
            const int frameY = row;
            const int chromaY = row / 2;
            const uint8_t *yRow = yPlane + frameY * yStride;
            const uint8_t *uRow = uPlane + chromaY * uStride;
            const uint8_t *vRow = vPlane + chromaY * vStride;

            for (int col = 0; col < 16; ++col)
            {
                const int frameX = mbX + col;
                const int chromaX = frameX / 2;

                const int Y = static_cast<int>(yRow[frameX]);
                const int U = static_cast<int>(uRow[chromaX]);
                const int V = static_cast<int>(vRow[chromaX]);

                const int lum = (149 * std::max(0, Y - 16)) >> 6;
                const int cr = V - 128;
                const int cb = U - 128;

                const uint8_t r = clampU8((lum + ((204 * cr) >> 6) + 1) >> 1);
                const uint8_t g = clampU8((lum + ((-104 * cr) >> 6) + ((-50 * cb) >> 6) + 1) >> 1);
                const uint8_t b = clampU8((lum + ((258 * cb) >> 6) + 1) >> 1);
                const uint8_t a = 0x80u;

                const int pixelOffset = (row * 16 + col) * 4;
                mbDst[pixelOffset + 0] = r;
                mbDst[pixelOffset + 1] = g;
                mbDst[pixelOffset + 2] = b;
                mbDst[pixelOffset + 3] = a;
            }
        }
    }

    av_frame_unref(s_frame);
    avcodec_flush_buffers(s_codecCtx);

    logResult(true);
    return true;
#endif
}
