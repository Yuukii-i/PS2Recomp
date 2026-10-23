#include <atomic>
std::atomic<uint16_t> g_hostPadButtons{0};
#ifdef _WIN32
#include <io.h>
#endif
#include "ps2_runtime.h"
#include "diag/DiagSampler.h"
std::atomic<uint32_t> g_diagMscalCount{0};
std::atomic<uint32_t> g_diagGifPacketCount{0};

extern "C" {
void push_trace(const char* msg) { (void)msg; }
void dump_trace() {}
}
#include "ps2_log.h"

#if AGRESSIVE_LOGS
namespace {
    struct Ps2LogDisabler {
        Ps2LogDisabler() {
            if (!std::getenv("PS2X_TRACE_LOG")) {
                ps2_log::log_stream().setstate(std::ios_base::badbit | std::ios_base::failbit);
            }
        }
    } s_ps2LogDisabler;
}
#endif
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "Kernel/Stubs/IsoDisc.h"
#include "Stubs/CD.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <cstdarg>

static std::mutex s_logMutex;

std::mutex &ps2xLogMutex()
{
    return s_logMutex;
}

static FILE *s_realStderrFile = nullptr;

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

class FilteredCoutStreambuf : public std::streambuf
{
    std::streambuf *m_orig = nullptr;
    std::string m_line;
    std::string m_lastMcLine;
    std::mutex m_mutex;

public:
    FilteredCoutStreambuf(std::streambuf *orig) : m_orig(orig) {}

    int overflow(int c) override
    {
        if (c == EOF) return !EOF;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (c == '\n')
        {
            processLine(m_line);
            m_line.clear();
        }
        else
        {
            m_line.push_back(static_cast<char>(c));
        }
        return c;
    }

    void processLine(const std::string &line)
    {
        // 1. [MC] line: print only when its values change
        if (line.rfind("[MC]", 0) == 0)
        {
            size_t dmaPos = line.find("[sceDmaSend");
            std::string mcPart = (dmaPos != std::string::npos) ? line.substr(0, dmaPos) : line;
            if (mcPart != m_lastMcLine)
            {
                m_lastMcLine = mcPart;
                if (m_orig)
                {
                    m_orig->sputn(mcPart.c_str(), mcPart.size());
                    if (isVerboseLog() && dmaPos != std::string::npos)
                    {
                        m_orig->sputn(line.c_str() + dmaPos, line.size() - dmaPos);
                    }
                    m_orig->sputc('\n');
                    m_orig->pubsync();
                }
            }
            else if (isVerboseLog() && dmaPos != std::string::npos && m_orig)
            {
                m_orig->sputn(line.c_str() + dmaPos, line.size() - dmaPos);
                m_orig->sputc('\n');
                m_orig->pubsync();
            }
            return;
        }

        // 2. [sceDmaSend] / [sceDmaSend:head]: gate behind PS2X_VERBOSE_LOG
        if (line.rfind("[sceDmaSend", 0) == 0)
        {
            if (isVerboseLog() && m_orig)
            {
                m_orig->sputn(line.c_str(), line.size());
                m_orig->sputc('\n');
                m_orig->pubsync();
            }
            return;
        }

        // Other cout lines: output normally
        if (m_orig)
        {
            m_orig->sputn(line.c_str(), line.size());
            m_orig->sputc('\n');
            m_orig->pubsync();
        }
    }

    int sync() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_line.empty())
        {
            processLine(m_line);
            m_line.clear();
        }
        return m_orig ? m_orig->pubsync() : 0;
    }
};

static void initCoutFilter()
{
    static bool s_init = false;
    if (s_init) return;
    s_init = true;
    static FilteredCoutStreambuf s_filteredCout(std::cout.rdbuf());
    std::cout.rdbuf(&s_filteredCout);
}

// Global stall diagnostics tracking
static std::array<uint32_t, 5> g_lastDistinctFns = {};
static std::atomic<bool> g_mpegFunctionCalled{false};
std::atomic<uint32_t> g_trackedCurScene{0};
std::atomic<uint32_t> g_trackedCurVtable{0};
std::atomic<bool> g_movieSeekDone{false};
std::atomic<uint32_t> g_movieReadsCount{0};
std::atomic<uint32_t> g_movieGetPictureCount{0};
std::atomic<uint32_t> g_movieIsEndCount{0};
std::atomic<bool> g_movieFallbackTriggered{false};
std::atomic<uint32_t> g_movieSceneFrames{0};
extern std::atomic<uint64_t> g_profGsUs;
extern std::atomic<uint64_t> g_profXferUs;
extern std::atomic<uint64_t> g_profPrims;
extern std::atomic<uint64_t> g_profPixels;
std::atomic<uint64_t> g_profEeUs{0};
static uint64_t s_profPresentUs = 0;
static auto g_lastSceneTransitionTime = std::chrono::steady_clock::now();

static void initStderrRedirection()
{
    static bool s_init = false;
    if (s_init) return;
    s_init = true;
    initCoutFilter();
#ifdef _WIN32
    if (!isVerboseLog())
    {
        int realStderrFd = _dup(2);
        if (realStderrFd >= 0)
        {
            s_realStderrFile = _fdopen(realStderrFd, "w");
            FILE *nul = std::fopen("NUL", "w");
            if (nul)
            {
                _dup2(_fileno(nul), 2);
                std::fclose(nul);
            }
        }
    }
#endif
}

void ps2xSafeLog(const char *fmt, ...)
{
    initStderrRedirection();
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
    {
        size_t writeLen = (len < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(len) : (sizeof(buf) - 1);
        std::lock_guard<std::mutex> lock(s_logMutex);
        FILE *target = s_realStderrFile ? s_realStderrFile : stderr;
        std::fwrite(buf, 1, writeLen, target);
        std::fflush(target);
    }
}

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *hModule, char *lpFilename, unsigned long nSize);
#endif

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight, uint64_t renderTick = 0u)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const uint64_t dumpTick = (renderTick != 0u) ? renderTick : currentTick;

    // Pad injection for headless progression / automated runs (executed unconditionally every frame)
    {
        struct PadInj {
            std::string button;
            uint16_t mask = 0;
            uint64_t startMs = 0;
            uint64_t durMs = 0;
            bool active = false;
        };
        static std::vector<PadInj> s_injections;
        static bool s_parsed = false;
        static auto s_bootTime = std::chrono::steady_clock::now();
        if (!s_parsed) {
            s_parsed = true;
            const char *env = std::getenv("PS2X_INJECT_PAD");
            if (env && env[0] != '\0') {
                std::string str(env);
                size_t pos = 0;
                while (pos < str.size()) {
                    size_t comma = str.find(',', pos);
                    std::string item = str.substr(pos, (comma == std::string::npos) ? std::string::npos : (comma - pos));
                    pos = (comma == std::string::npos) ? str.size() : (comma + 1);
                    size_t c1 = item.find(':');
                    size_t c2 = (c1 != std::string::npos) ? item.find(':', c1 + 1) : std::string::npos;
                    if (c1 != std::string::npos && c2 != std::string::npos) {
                        PadInj inj;
                        inj.button = item.substr(0, c1);
                        try {
                            inj.startMs = std::stoull(item.substr(c1 + 1, c2 - c1 - 1));
                            inj.durMs = std::stoull(item.substr(c2 + 1));
                        } catch (...) { continue; }
                        if (inj.button == "START") inj.mask = 1u << 3;
                        else if (inj.button == "CROSS") inj.mask = 1u << 14;
                        else if (inj.button == "CIRCLE") inj.mask = 1u << 13;
                        else if (inj.button == "TRIANGLE") inj.mask = 1u << 12;
                        else if (inj.button == "SQUARE") inj.mask = 1u << 15;
                        if (inj.mask != 0) s_injections.push_back(inj);
                    }
                }
            }
        }

        uint16_t activeMask = 0;
        if (!s_injections.empty()) {
            const uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_bootTime).count();
            for (auto &inj : s_injections) {
                bool shouldBeActive = (nowMs >= inj.startMs && nowMs < (inj.startMs + inj.durMs));
                if (shouldBeActive != inj.active) {
                    inj.active = shouldBeActive;
                    ps2xSafeLog("[pad:inject] %s %s at %llums (tick=%llu)\n",
                                inj.button.c_str(), shouldBeActive ? "DOWN" : "UP",
                                (unsigned long long)nowMs, (unsigned long long)currentTick);
                    std::fflush(stderr);
                }
                if (inj.active) activeMask |= inj.mask;
            }
        } else {
            static int s_autoPadUpload = -1;
            if (s_autoPadUpload == -1) {
                const char *env = std::getenv("PS2X_AUTO_PAD");
                s_autoPadUpload = (env && (env[0] == '1' || env[0] == 't')) ? 1 : 0;
            }
            if (s_autoPadUpload && currentTick >= 68 && currentTick < 300) {
                bool pressConfirm = ((currentTick % 20) < 10);
                if (pressConfirm) {
                    activeMask |= (1u << 14) | (1u << 13); // CROSS + CIRCLE
                }
            }
        }

        if (activeMask != 0) {
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~activeMask), 0x80, 0x80, 0x80, 0x80);
        }
    }

    int dumpInterval = 0;
    const char *dumpFramesCheck = std::getenv("PS2X_DUMP_FRAMES");
    if (dumpFramesCheck && dumpFramesCheck[0] != '\0')
    {
        try { dumpInterval = std::stoi(dumpFramesCheck); } catch (...) { dumpInterval = 0; }
    }
    const bool isDumpScheduled = (dumpTick == 1u || dumpTick == 10u || dumpTick == 100u || (dumpInterval > 0 && (dumpTick % static_cast<uint64_t>(dumpInterval) == 0u)));

    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick || isDumpScheduled;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    uint32_t frameHash = 2166136261u;
    bool hasNonZeroPixels = false;
    for (size_t bi = 0; bi < s_scratch.size(); bi += 32)
    {
        frameHash = (frameHash ^ s_scratch[bi]) * 16777619u;
        if (s_scratch[bi] != 0 || (bi + 1 < s_scratch.size() && s_scratch[bi + 1] != 0) || (bi + 2 < s_scratch.size() && s_scratch[bi + 2] != 0))
        {
            hasNonZeroPixels = true;
        }
    }

    if ((currentTick >= 68 && currentTick <= 75) || frameHash == 0xefb69dc5u)
    {
        static bool s_tick70Dumped = false;
        if (!s_tick70Dumped && !s_scratch.empty() && width != 0 && height != 0)
        {
            s_tick70Dumped = true;
            extern void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename);
            std::filesystem::create_directories("frames");
            writeUncompressedPng(width, height, s_scratch.data(), "frames/frame_tick70.png");
            ps2xSafeLog("[frame:dump] Dumped frame at tick=%llu hash=0x%08x to frames/frame_tick70.png\n",
                        (unsigned long long)currentTick, frameHash);
        }
    }

    static uint32_t s_publogoHash = 0;
    const char *dumpFramesEnv = std::getenv("PS2X_DUMP_FRAMES");
    const bool allowDumpFrames = (dumpFramesEnv && std::strcmp(dumpFramesEnv, "1") == 0);
    if (allowDumpFrames && hasNonZeroPixels && !s_scratch.empty() && width != 0 && height != 0)
    {
        static uint64_t s_lastLiveDumpTick = 0;
        if (currentTick != s_lastLiveDumpTick && currentTick >= 120 && currentTick <= 170)
        {
            s_lastLiveDumpTick = currentTick;
            extern void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename);
            std::filesystem::create_directories("frames");
            std::string name = "frames/frame_live_" + std::to_string(currentTick) + ".png";
            writeUncompressedPng(width, height, s_scratch.data(), name.c_str());
            ps2xSafeLog("[frame:dump] Dumped live frame at tick=%llu hash=0x%08x to %s\n",
                        (unsigned long long)currentTick, frameHash, name.c_str());
        }
    }

    if (currentTick >= 85 && currentTick <= 250)
    {
        static bool s_publogoDumped = false;
        if (!s_publogoDumped && !s_scratch.empty() && width != 0 && height != 0 && hasNonZeroPixels)
        {
            s_publogoDumped = true;
            s_publogoHash = frameHash;
            extern void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename);
            std::filesystem::create_directories("frames");
            writeUncompressedPng(width, height, s_scratch.data(), "frames/frame_publogo.png");
            ps2xSafeLog("[frame:dump] Dumped publogo at tick=%llu hash=0x%08x to frames/frame_publogo.png\n",
                        (unsigned long long)currentTick, frameHash);
        }
    }

    if (currentTick >= 380)
    {
        static uint64_t s_lastPostSceaDump = 0;
        if (currentTick >= s_lastPostSceaDump + 30 && !s_scratch.empty() && width != 0 && height != 0 && hasNonZeroPixels)
        {
            s_lastPostSceaDump = currentTick;
            extern void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename);
            std::filesystem::create_directories("frames");
            std::string name = "frames/frame_post_scea_" + std::to_string(currentTick) + ".png";
            writeUncompressedPng(width, height, s_scratch.data(), name.c_str());
            ps2xSafeLog("[frame:dump] Dumped post-SCEA frame at tick=%llu hash=0x%08x to %s\n",
                        (unsigned long long)currentTick, frameHash, name.c_str());
        }
    }
    if (currentTick >= 400)
    {
        static uint32_t s_lastTitleHash = 0;
        if (frameHash != s_publogoHash && frameHash != s_lastTitleHash && !s_scratch.empty() && width != 0 && height != 0 && hasNonZeroPixels)
        {
            s_lastTitleHash = frameHash;
            extern void writeUncompressedPng(uint32_t w, uint32_t h, const uint8_t *rgba, const char *filename);
            std::filesystem::create_directories("frames");
            writeUncompressedPng(width, height, s_scratch.data(), "frames/frame_title.png");
            ps2xSafeLog("[frame:dump] Dumped title screen at tick=%llu (hasNonZero=%d) hash=0x%08x to frames/frame_title.png\n",
                        (unsigned long long)currentTick, hasNonZeroPixels ? 1 : 0, frameHash);
        }
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (isVerboseLog() && (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight))
        {
            ps2xSafeLog("[frame:upload] idx=%u tick=%llu displayFbp=%u sourceFbp=%u size=%ux%u preferred=%u hash=0x%08x\n",
                        s_uploadDebugCount,
                        (unsigned long long)currentTick,
                        displayFbp,
                        sourceFbp,
                        width,
                        height,
                        usedPreferredDisplaySource ? 1u : 0u,
                        frameHash);
        }
        ++s_uploadDebugCount;
        if (isVerboseLog() && s_uploadDebugCount == 128u)
        {
            ps2xSafeLog("[log] cap reached for tag frame:upload at idx 128\n");
            std::fflush(stderr);
        }
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
    ps2_diag::DiagSampler::onFrame(rt, currentTick, s_uploadBuffer, displayFbp, sourceFbp, width, height);

    static uint64_t s_lastDumpTick = std::numeric_limits<uint64_t>::max();
    static uint32_t s_frameDumpCount = 0u;
    static uint32_t s_vramDumpCount = 0u;

    if (dumpTick != s_lastDumpTick)
    {
        s_lastDumpTick = dumpTick;
        const char *dumpFramesEnv = std::getenv("PS2X_DUMP_FRAMES");
        if (dumpFramesEnv && dumpFramesEnv[0] != '\0' && s_frameDumpCount < 20u)
        {
            int n = 0;
            try { n = std::stoi(dumpFramesEnv); } catch (...) { n = 0; }
            if (dumpTick == 1u || dumpTick == 10u || dumpTick == 100u || (n > 0 && (dumpTick % static_cast<uint64_t>(n) == 0u)))
            {
                std::filesystem::create_directories("frames");
                const std::string framePath = "frames/frame_" + std::to_string(dumpTick) + ".png";
                const uint32_t dumpW = FB_WIDTH;
                const uint32_t dumpH = (height != 0u) ? std::min<uint32_t>(height, FB_HEIGHT) : DEFAULT_DISPLAY_HEIGHT;

                uint32_t nonBlackPixels = 0u;
                std::vector<uint8_t> dumpPixels(dumpW * dumpH * 4u);
                for (uint32_t y = 0; y < dumpH; ++y)
                {
                    const uint8_t *srcRow = s_uploadBuffer.data() + y * FB_WIDTH * 4u;
                    uint8_t *dstRow = dumpPixels.data() + y * dumpW * 4u;
                    for (uint32_t x = 0; x < dumpW; ++x)
                    {
                        const uint8_t r = srcRow[x * 4u + 0];
                        const uint8_t g = srcRow[x * 4u + 1];
                        const uint8_t b = srcRow[x * 4u + 2];
                        const uint8_t a = srcRow[x * 4u + 3];
                        dstRow[x * 4u + 0] = r;
                        dstRow[x * 4u + 1] = g;
                        dstRow[x * 4u + 2] = b;
                        dstRow[x * 4u + 3] = (a != 0u) ? a : 255u;
                        if (r != 0u || g != 0u || b != 0u)
                        {
                            ++nonBlackPixels;
                        }
                    }
                }
                Image frameImg{};
                frameImg.data = dumpPixels.data();
                frameImg.width = static_cast<int>(dumpW);
                frameImg.height = static_cast<int>(dumpH);
                frameImg.mipmaps = 1;
                frameImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                ExportImage(frameImg, framePath.c_str());

                const auto &regs = rt->memory().gs();
                ps2xSafeLog( "[frame-dump] tick=%llu width=%u height=%u non_black=%u DISPFB1=0x%016llx DISPLAY1=0x%016llx DISPFB2=0x%016llx DISPLAY2=0x%016llx\n",
                             static_cast<unsigned long long>(dumpTick),
                             dumpW,
                             dumpH,
                             nonBlackPixels,
                             static_cast<unsigned long long>(regs.dispfb1),
                             static_cast<unsigned long long>(regs.display1),
                             static_cast<unsigned long long>(regs.dispfb2),
                             static_cast<unsigned long long>(regs.display2));
                std::fflush(stderr);
                ++s_frameDumpCount;
            }
        }

        const char *dumpVramEnv = std::getenv("PS2X_DUMP_VRAM");
        if (dumpVramEnv && std::strcmp(dumpVramEnv, "1") == 0 && s_vramDumpCount < 20u)
        {
            int n = 0;
            const char *dumpFramesEnv = std::getenv("PS2X_DUMP_FRAMES");
            if (dumpFramesEnv && dumpFramesEnv[0] != '\0')
            {
                try { n = std::stoi(dumpFramesEnv); } catch (...) { n = 0; }
            }
            if (dumpTick == 1u || dumpTick == 10u || dumpTick == 100u || (n > 0 && (dumpTick % static_cast<uint64_t>(n) == 0u)))
            {
                std::filesystem::create_directories("frames");
                const std::string vramPath = "frames/vram_" + std::to_string(dumpTick) + ".png";
                std::vector<uint8_t> vramRgba(1024 * 1024 * 4, 255);
                uint32_t vramNonBlack = 0u;
                const uint32_t baseBlock = displayFbp * 32u;
                for (uint32_t vy = 0; vy < 1024; ++vy)
                {
                    for (uint32_t vx = 0; vx < 1024; ++vx)
                    {
                        const uint32_t val = rt->gs().ReadVram(GS_PSM_CT32, baseBlock, 16u, vx, vy);
                        const uint8_t r = static_cast<uint8_t>(val & 0xFFu);
                        const uint8_t g = static_cast<uint8_t>((val >> 8u) & 0xFFu);
                        const uint8_t b = static_cast<uint8_t>((val >> 16u) & 0xFFu);
                        const size_t idx = (static_cast<size_t>(vy) * 1024u + vx) * 4u;
                        vramRgba[idx + 0] = r;
                        vramRgba[idx + 1] = g;
                        vramRgba[idx + 2] = b;
                        vramRgba[idx + 3] = 255u;
                        if (r != 0u || g != 0u || b != 0u)
                        {
                            ++vramNonBlack;
                        }
                    }
                }
                Image vramImg{};
                vramImg.data = vramRgba.data();
                vramImg.width = 1024;
                vramImg.height = 1024;
                vramImg.mipmaps = 1;
                vramImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                ExportImage(vramImg, vramPath.c_str());

                ps2xSafeLog( "[vram-dump] tick=%llu displayFbp=%u baseBlock=%u non_black=%u path=%s\n",
                             static_cast<unsigned long long>(dumpTick),
                             displayFbp,
                             baseBlock,
                             vramNonBlack,
                             vramPath.c_str());
                std::fflush(stderr);
                ++s_vramDumpCount;
            }
        }
    }
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    // The main thread stack occupies [0x01FA0000, 0x02000000] (SetupThread: stack base=0x01FA0000, size=0x60000).
    // Async callback stacks MUST NOT collide with the main thread's live stack frames.
    // They are placed below the main stack in [0x01F00000, 0x01FA0000].
    constexpr uint32_t kDefaultAsyncCallbackStackTop = 0x01FA0000u;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = kDefaultAsyncCallbackStackTop;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { g_diagGifPacketCount.fetch_add(1, std::memory_order_relaxed); m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     g_diagMscalCount.fetch_add(1, std::memory_order_relaxed);
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

static void registerConfigStubs(PS2Runtime &runtime, const std::string &configPath);

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = 0x01FA0000u;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    // 1. Generic config.toml stub registration
    registerConfigStubs(*this, "config.toml");

    // 0x003703D4: Early exit check returning non-zero error code -> force return 0
    replaceFunction(0x003703D4u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        const char *exdb = std::getenv("PS2X_TRACE_EXDB");
        if (exdb && std::strcmp(exdb, "1") == 0) {
            ps2xSafeLog("[override] 0x003703D4 called (ra=0x%08x) -> returning 0\n", getRegU32(ctx, 31));
        }
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
    });

    // 0x003DF928: UTF-8 BOM/3-byte check helper (missing secondary entry in sub_003DF728)
    replaceFunction(0x003DF928u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        (void)runtime;
        const uint32_t a1 = getRegU32(ctx, 5);
        const uint8_t *p = getMemPtr(rdram, a1);
        if (!p)
        {
            setReturnU32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        const uint8_t b0 = p[0];
        const uint8_t b1 = p[1];
        const uint8_t b2 = p[2];

        if ((b2 & 0x80u) == 0)
        {
            setReturnU32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }

        uint32_t res = 0;
        const uint8_t t6_c0 = b2 & 0xC0u;

        if (b0 == 0xEFu && b1 == 0xBFu)
        {
            if (b2 >= 0xBEu)
            {
                setReturnU32(ctx, 1);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        else if (t6_c0 == 0xC0u)
        {
            setReturnU32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }

        if (b0 == 0xE0u)
        {
            if (b1 < 0xA0u || (b1 & 0xC0u) == 0xC0u)
                res = 1;
        }
        else
        {
            if ((b1 & 0x80u) == 0)
            {
                res = 1;
            }
            else if (b0 == 0xEDu)
            {
                if (b1 >= 0xA0u)
                    res = 1;
            }
            else
            {
                if ((b1 & 0xC0u) == 0xC0u)
                    res = 1;
            }
        }

        setReturnU32(ctx, res);
        ctx->pc = getRegU32(ctx, 31);
    });

    // 0x003DFA08: UTF-8 4-byte check helper (missing secondary entry in sub_003DF728)
    replaceFunction(0x003DFA08u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        (void)runtime;
        const uint32_t a1 = getRegU32(ctx, 5);
        const uint8_t *p = getMemPtr(rdram, a1);
        if (!p)
        {
            setReturnU32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        const uint8_t b0 = p[0];
        const uint8_t b1 = p[1];
        const uint8_t b2 = p[2];
        const uint8_t b3 = p[3];

        if ((b3 & 0x80u) == 0 || (b3 & 0xC0u) == 0xC0u ||
            (b2 & 0x80u) == 0 || (b2 & 0xC0u) == 0xC0u)
        {
            setReturnU32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }

        uint32_t res = 0;
        if (b0 == 0xF0u)
        {
            if (b1 < 0x90u || (b1 & 0xC0u) == 0xC0u)
                res = 1;
        }
        else
        {
            if ((b1 & 0x80u) == 0)
            {
                res = 1;
            }
            else if (b0 != 0xF4u)
            {
                if ((b1 & 0xC0u) == 0xC0u)
                    res = 1;
            }
            else
            {
                if (b1 < 0x90u || (b1 & 0xC0u) == 0xC0u)
                    res = 1;
            }
        }

        setReturnU32(ctx, res);
        ctx->pc = getRegU32(ctx, 31);
    });

    // 0x003DF4B8: Exit/tail-call stub calling 0x003DF700 and deallocating 16 bytes stack
    replaceFunction(0x003DF4B8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t sp = getRegU32(ctx, 29);
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(sp + 16))); // $sp += 16
        ctx->r[15] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(0x003DF6F8u))); // $t7 = 0x003df6f8
        ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(-1)); // $v0 = -1
        if (a0 != 0 && (a0 + 4) <= PS2_RAM_SIZE) {
            *reinterpret_cast<uint32_t*>(rdram + (a0 & PS2_RAM_MASK)) = 0x003DF6F8u;
        }
        ctx->pc = ra;
    });

    // 0x003DF6F8: Return 0 stub
    replaceFunction(0x003DF6F8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        (void)rdram; (void)runtime;
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
    });

    // 0x003DF700: Stub setting v0=-1, writing 0x003DF6F8 to 0($a0), jr $ra
    replaceFunction(0x003DF700u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        (void)runtime;
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        ctx->r[15] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(0x003DF6F8u))); // $t7 = 0x003df6f8
        ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(-1)); // $v0 = -1
        if (a0 != 0 && (a0 + 4) <= PS2_RAM_SIZE) {
            *reinterpret_cast<uint32_t*>(rdram + (a0 & PS2_RAM_MASK)) = 0x003DF6F8u;
        }
        ctx->pc = ra;
    });

    // 0x001E39D4: Native replacement for secondary entry in sub_001E39CC (color unpacking)
    replaceFunction(0x001E39D4u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        (void)runtime;
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint32_t ra = getRegU32(ctx, 31);
        if (a0 != 0 && (a0 + 0x90) <= PS2_RAM_SIZE) {
            const uint32_t t6 = *reinterpret_cast<const uint32_t *>(rdram + (a0 + 0x5Cu));
            const uint32_t t4 = (t6 >> 24) & 0xFFu;
            const uint32_t t5 = (t6 >> 16) & 0xFFu;
            const uint32_t t7 = (t6 >> 8) & 0xFFu;
            const uint32_t t6_b = t6 & 0xFFu;
            *reinterpret_cast<uint32_t *>(rdram + (a0 + 0x88u)) = 0x80u;
            *reinterpret_cast<uint32_t *>(rdram + (a0 + 0x64u)) = t4;
            *reinterpret_cast<uint32_t *>(rdram + (a0 + 0x84u)) = t4;
            *reinterpret_cast<uint32_t *>(rdram + (a0 + 0x7Cu)) = t7;
            *reinterpret_cast<uint32_t *>(rdram + (a0 + 0x80u)) = t5;
            *reinterpret_cast<uint32_t *>(rdram + (a0 + 0x78u)) = t6_b;
            ctx->r[12] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(t4)));
            ctx->r[13] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(t5)));
            ctx->r[14] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(t6_b)));
            ctx->r[15] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(t7)));
        }
        ctx->pc = ra;
    });

    // 0x00408E90: sceMpegIsEnd in game ELF (called by sub_0031A1B4:264 to test movie end)
    if (0x00408E90u >= g_ps2RecompiledFunctionTableBase && 0x00408E90u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot408e90 = (0x00408E90u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig408e90 = g_ps2RecompiledFunctionTable[slot408e90];
        replaceFunction(0x00408E90u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            g_movieIsEndCount.fetch_add(1, std::memory_order_relaxed);
            static const char *fallbackEnv = std::getenv("PS2X_MOVIE_FALLBACK");
            static const bool s_movieFallback = (fallbackEnv && (fallbackEnv[0] == '1' || fallbackEnv[0] == 't'));
            if (s_movieFallback && g_trackedCurVtable.load(std::memory_order_relaxed) == 0x006adfa8u && g_movieSceneFrames.load(std::memory_order_relaxed) >= 120) {
                g_movieFallbackTriggered.store(true, std::memory_order_relaxed);
                static bool s_warned = false;
                if (!s_warned) {
                    s_warned = true;
                    ps2xSafeLog("[movie] PS2X_MOVIE_FALLBACK triggered at frame %u: 0x00408E90 returning 1\n", g_movieSceneFrames.load());
                }
                setReturnS32(ctx, 1);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            if (s_orig408e90)
            {
                s_orig408e90(rdram, ctx, runtime);
            }
            else
            {
                setReturnS32(ctx, 0);
                ctx->pc = getRegU32(ctx, 31);
            }
        });
    }

    // 0x00408D28: guest MPEG frame decode call in sub_0031A1B4:121
    if (0x00408D28u >= g_ps2RecompiledFunctionTableBase && 0x00408D28u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot408d28 = (0x00408D28u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig408d28 = g_ps2RecompiledFunctionTable[slot408d28];
        replaceFunction(0x00408D28u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::sceMpegGetPicture(rdram, ctx, runtime);
            if (s_orig408d28)
            {
                s_orig408d28(rdram, ctx, runtime);
            }
            else
            {
                setReturnS32(ctx, 1);
                ctx->pc = getRegU32(ctx, 31);
            }
        });
    }

    // Trace sub_0052DFA8 (Update method of 0x006B0110 container)
    if (0x0052DFA8u >= g_ps2RecompiledFunctionTableBase && 0x0052DFA8u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot52dfa8 = (0x0052DFA8u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig52dfa8 = g_ps2RecompiledFunctionTable[slot52dfa8];
        replaceFunction(0x0052DFA8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static int s_count = 0;
            if (++s_count <= 5 || s_count % 60 == 0)
            {
                uint32_t a0 = getRegU32(ctx, 4);
                uint32_t child = (a0 != 0 && a0 + 0x34 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + a0 + 0x30) : 0;
                uint32_t childVtable = (child != 0 && child + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + child) : 0;
                ps2xSafeLog("[scene] sub_0052DFA8 called #%d a0=0x%08x child=0x%08x (vtable=0x%08x)\n", s_count, a0, child, childVtable);
            }
            if (s_orig52dfa8) s_orig52dfa8(rdram, ctx, runtime);
        });
    }

    // Trace sub_004E914C (Publogo update)
    if (0x004E914Cu >= g_ps2RecompiledFunctionTableBase && 0x004E914Cu < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot4e914c = (0x004E914Cu - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig4e914c = g_ps2RecompiledFunctionTable[slot4e914c];
        replaceFunction(0x004E914Cu, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static int s_count = 0;
            if (++s_count <= 5 || s_count % 60 == 0)
            {
                ps2xSafeLog("[scene] sub_004E914C called #%d\n", s_count);
            }
            if (s_orig4e914c) s_orig4e914c(rdram, ctx, runtime);
        });
    }

    // Publogo timer acceleration (behind env PS2X_HACK_FF_PUBLOGO=1, default OFF)
    if (0x004E9278u >= g_ps2RecompiledFunctionTableBase && 0x004E9278u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot4e9278 = (0x004E9278u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig4e9278 = g_ps2RecompiledFunctionTable[slot4e9278];
        replaceFunction(0x004E9278u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static int s_publogoFrames = 0;
            s_publogoFrames++;
            uint32_t a0 = getRegU32(ctx, 4);
            const char *ffEnv = std::getenv("PS2X_HACK_FF_PUBLOGO");
            if (ffEnv && std::strcmp(ffEnv, "1") == 0)
            {
                if (s_publogoFrames >= 3 && a0 != 0 && a0 + 8 <= PS2_RAM_SIZE)
                {
                    ps2xSafeLog("[scene] Fast-forwarding Publogo timer at frame %d (a0=0x%08x)...\n", s_publogoFrames, a0);
                    *reinterpret_cast<float *>(rdram + a0 + 4) = 6.25f;
                }
            }
            if (s_orig4e9278) s_orig4e9278(rdram, ctx, runtime);
        });
    }

    // Diagnostics for movie functions (sub_0031A1B4, 0x0031A240)
    if (0x0031A1B4u >= g_ps2RecompiledFunctionTableBase && 0x0031A1B4u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot = (0x0031A1B4u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig31a1b4 = g_ps2RecompiledFunctionTable[slot];
        replaceFunction(0x0031A1B4u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; ps2xSafeLog("[movie:diag] sub_0031A1B4 called!\n"); }
            g_mpegFunctionCalled.store(true, std::memory_order_relaxed);
            if (s_orig31a1b4) s_orig31a1b4(rdram, ctx, runtime);
        });
    }
    if (0x0031A240u >= g_ps2RecompiledFunctionTableBase && 0x0031A240u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot = (0x0031A240u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig31a240 = g_ps2RecompiledFunctionTable[slot];
        replaceFunction(0x0031A240u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; ps2xSafeLog("[movie:diag] 0x0031A240 called!\n"); }
            g_mpegFunctionCalled.store(true, std::memory_order_relaxed);
            if (s_orig31a240) s_orig31a240(rdram, ctx, runtime);
        });
    }

    // SceneManager (sub_0015A488) transition tracker
    if (0x0015A488u >= g_ps2RecompiledFunctionTableBase && 0x0015A488u < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t slot15a488 = (0x0015A488u - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_orig15a488 = g_ps2RecompiledFunctionTable[slot15a488];
        replaceFunction(0x0015A488u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            uint32_t sm = getRegU32(ctx, 4); // a0 = SceneManager
            static uint32_t s_lastCur = 0;
            static uint32_t s_lastNext = 0;
            if (sm != 0 && sm + 8 <= PS2_RAM_SIZE)
            {
                uint32_t curScene = *reinterpret_cast<const uint32_t *>(rdram + sm);
                uint32_t nextScene = *reinterpret_cast<const uint32_t *>(rdram + sm + 4);
                if (curScene != s_lastCur || nextScene != s_lastNext)
                {
                    uint32_t curVtable = (curScene != 0 && curScene + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + curScene) : 0;
                    uint32_t nextVtable = (nextScene != 0 && nextScene + 4 <= PS2_RAM_SIZE) ? *reinterpret_cast<const uint32_t *>(rdram + nextScene) : 0;
                    ps2xSafeLog("[scene] SceneManager transition: sm=0x%08x cur=0x%08x (vtable=0x%08x) next=0x%08x (vtable=0x%08x)\n",
                                sm, curScene, curVtable, nextScene, nextVtable);
                    s_lastCur = curScene;
                    s_lastNext = nextScene;
                    g_trackedCurScene.store(curScene, std::memory_order_relaxed);
                    g_trackedCurVtable.store(curVtable, std::memory_order_relaxed);
                    g_lastSceneTransitionTime = std::chrono::steady_clock::now();
                    if (curVtable == 0x006adfa8u) {
                        g_movieSceneFrames.store(0, std::memory_order_relaxed);
                        g_movieFallbackTriggered.store(false, std::memory_order_relaxed);
                    }
                }
            }
            if (s_orig15a488) s_orig15a488(rdram, ctx, runtime);
        });
    }

    // Logging for sub_0015993C:
    if (0x0015993Cu >= g_ps2RecompiledFunctionTableBase && 0x0015993Cu < g_ps2RecompiledFunctionTableEnd)
    {
        const uint32_t sub15993cSlot = (0x0015993Cu - g_ps2RecompiledFunctionTableBase) >> 2;
        static RecompiledFunction s_origSub15993c = g_ps2RecompiledFunctionTable[sub15993cSlot];
        replaceFunction(0x0015993Cu, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static bool s_firstLogged = false;
            if (!s_firstLogged)
            {
                s_firstLogged = true;
                const uint8_t val69981c = *reinterpret_cast<const uint8_t *>(rdram + 0x0069981Cu);
                const uint32_t val649714 = *reinterpret_cast<const uint32_t *>(rdram + 0x00649714u);
                ps2xSafeLog( "[sub_0015993C:first_call] *(u8*)0x0069981C=0x%02x, *(u32*)0x00649714=0x%08x\n",
                        val69981c, val649714);
                fflush(stderr);
            }
            if (s_origSub15993c)
            {
                s_origSub15993c(rdram, ctx, runtime);
            }
        });
    }

    // Syscall wrappers called via jump (J) instructions not emitted into recompiled table:
    static auto wrapperSyscall2 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        ps2xSafeLog( "[syscall-wrapper] Dispatching syscall 2 at pc=0x%08x (ra=0x%08x)\n", ctx->pc, getRegU32(ctx, 31));
        fflush(stderr);
        ps2_syscalls::dispatchNumericSyscall(2, rdram, ctx, runtime);
        ctx->pc = getRegU32(ctx, 31);
    };
    static auto wrapperSyscall4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t sp = getRegU32(ctx, 29);
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t v0 = getRegU32(ctx, 2);
        ps2xSafeLog("[syscall-wrapper] Dispatching syscall 4 (ExitThread) at pc=0x%08x ra=0x%08x sp=0x%08x a0=0x%08x a1=0x%08x v0=0x%08x\n",
                    ctx->pc, ra, sp, a0, a1, v0);
        if (sp >= 0x100000 && sp < 0x01fffe00) {
            uint32_t dumpEnd = std::min(sp + 0x2000, 0x01fffe00u);
            for (uint32_t off = sp; off < dumpEnd; off += 16) {
                uint32_t w0 = *reinterpret_cast<const uint32_t *>(rdram + off);
                uint32_t w1 = *reinterpret_cast<const uint32_t *>(rdram + off + 4);
                uint32_t w2 = *reinterpret_cast<const uint32_t *>(rdram + off + 8);
                uint32_t w3 = *reinterpret_cast<const uint32_t *>(rdram + off + 12);
                if (w0 != 0 || w1 != 0 || w2 != 0 || w3 != 0) {
                    ps2xSafeLog("  [stack 0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x\n", off, w0, w1, w2, w3);
                }
            }
        }
        fflush(stderr);
        ps2_syscalls::dispatchNumericSyscall(4, rdram, ctx, runtime);
        ctx->pc = getRegU32(ctx, 31);
    };
    static auto wrapperSyscall6 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        ps2xSafeLog( "[syscall-wrapper] Dispatching syscall 6 at pc=0x%08x (ra=0x%08x)\n", ctx->pc, getRegU32(ctx, 31));
        fflush(stderr);
        ps2_syscalls::dispatchNumericSyscall(6, rdram, ctx, runtime);
        ctx->pc = getRegU32(ctx, 31);
    };
    replaceFunction(0x004356E0u, wrapperSyscall2);
    replaceFunction(0x00435700u, wrapperSyscall4);
    replaceFunction(0x00435720u, wrapperSyscall6);

    const char *traceExdbEnv = std::getenv("PS2X_TRACE_EXDB");
    if (traceExdbEnv && std::strcmp(traceExdbEnv, "1") == 0)
    {
    const uint32_t slot36ef00 = (0x0036EF00u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig36ef00 = g_ps2RecompiledFunctionTable[slot36ef00];
    replaceFunction(0x0036EF00u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t size = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig36ef00) s_orig36ef00(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[MemoryManager:allocate_dlmalloc] size=0x%x (%u) ra=0x%08x -> ret=0x%08x\n", size, size, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot36edac = (0x0036EDACu - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig36edac = g_ps2RecompiledFunctionTable[slot36edac];
    replaceFunction(0x0036EDACu, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig36edac) s_orig36edac(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[func_36EDAC] a0=0x%x a1=0x%x a2=0x%x ra=0x%08x -> ret=0x%08x\n", a0, a1, a2, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot36ed64 = (0x0036ED64u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig36ed64 = g_ps2RecompiledFunctionTable[slot36ed64];
    replaceFunction(0x0036ED64u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig36ed64) s_orig36ed64(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[func_36ED64] a0=0x%x ra=0x%08x -> ret=0x%08x\n", a0, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot36e368 = (0x0036E368u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig36e368 = g_ps2RecompiledFunctionTable[slot36e368];
    replaceFunction(0x0036E368u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig36e368) s_orig36e368(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[func_36E368] a0=0x%x ra=0x%08x -> ret=0x%08x\n", a0, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot36ee4c = (0x0036EE4Cu - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig36ee4c = g_ps2RecompiledFunctionTable[slot36ee4c];
    replaceFunction(0x0036EE4Cu, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig36ee4c) s_orig36ee4c(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[func_36EE4C] a0=0x%x ra=0x%08x -> ret=0x%08x\n", a0, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot36e178 = (0x0036E178u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig36e178 = g_ps2RecompiledFunctionTable[slot36e178];
    replaceFunction(0x0036E178u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig36e178) s_orig36e178(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[func_36E178] a0=0x%x a1=0x%x a2=0x%x ra=0x%08x -> ret=0x%08x\n", a0, a1, a2, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot1014a8 = (0x001014A8u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig1014a8 = g_ps2RecompiledFunctionTable[slot1014a8];
    replaceFunction(0x001014A8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t incr = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig1014a8) s_orig1014a8(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[dlmalloc:MORECORE] incr=0x%x (%d) ra=0x%08x -> ret=0x%08x\n", incr, (int32_t)incr, ra, ret);
        fflush(stderr);
    });

    const uint32_t slot41b800 = (0x0041B800u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig41b800 = g_ps2RecompiledFunctionTable[slot41b800];
    replaceFunction(0x0041B800u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t size = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (s_orig41b800) s_orig41b800(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        if (ret == 0) {
            ps2xSafeLog("[operator_new:FAILED] size=0x%x (%u) ra=0x%08x -> ret=0x%08x\n", size, size, ra, ret);
            fflush(stderr);
        }
    });

    const uint32_t slot41b6a8 = (0x0041B6A8u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig41b6a8 = g_ps2RecompiledFunctionTable[slot41b6a8];
    replaceFunction(0x0041B6A8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t a2 = getRegU32(ctx, 6);
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t sp = getRegU32(ctx, 29);
        ps2xSafeLog("[cxa_throw:ENTER] obj=0x%08x typeinfo=0x%08x dtor=0x%08x ra=0x%08x sp=0x%08x\n", a0, a1, a2, ra, sp);
        fflush(stderr);
        if (s_orig41b6a8) s_orig41b6a8(rdram, ctx, runtime);
    });

    const uint32_t slot4159f8 = (0x004159F8u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig4159f8 = g_ps2RecompiledFunctionTable[slot4159f8];
    replaceFunction(0x004159F8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t strAddr = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t sp = getRegU32(ctx, 29);
        const char *msg = strAddr ? reinterpret_cast<const char *>(rdram + (strAddr & 0x01FFFFFFu)) : "<null>";
        ps2xSafeLog("[throw_exception:ENTER] msg=\"%s\" (strAddr=0x%08x) ra=0x%08x sp=0x%08x\n", msg, strAddr, ra, sp);
        fflush(stderr);
        if (s_orig4159f8) s_orig4159f8(rdram, ctx, runtime);
    });

    const uint32_t slot428b28 = (0x00428B28u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig428b28 = g_ps2RecompiledFunctionTable[slot428b28];
    replaceFunction(0x00428B28u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t sp = getRegU32(ctx, 29);
        ps2xSafeLog("[std::terminate:ENTER] ra=0x%08x sp=0x%08x\n", ra, sp);
        fflush(stderr);
        if (s_orig428b28) s_orig428b28(rdram, ctx, runtime);
    });

    const uint32_t slot1f28c4 = (0x001F28C4u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig1f28c4 = g_ps2RecompiledFunctionTable[slot1f28c4];
    replaceFunction(0x001F28C4u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4); // vector<string>*
        uint32_t a1 = getRegU32(ctx, 5); // string*
        uint32_t a2 = getRegU32(ctx, 6); // delimiter char
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t sp = getRegU32(ctx, 29);
        const char *strData = "<null>";
        if (a1 && (a1 & 0x01FFFFFFu) < 0x02000000u - 4) {
            uint32_t charPtr = *reinterpret_cast<const uint32_t *>(rdram + (a1 & 0x01FFFFFFu));
            if (charPtr && (charPtr & 0x01FFFFFFu) < 0x02000000u) {
                strData = reinterpret_cast<const char *>(rdram + (charPtr & 0x01FFFFFFu));
            }
        }
        ps2xSafeLog("[func_1F28C4:ENTER] vec=0x%08x strPtr=0x%08x (\"%s\") delim='%c'(0x%x) ra=0x%08x sp=0x%08x\n",
                    a0, a1, strData, (char)a2, a2, ra, sp);
        fflush(stderr);
        if (s_orig1f28c4) s_orig1f28c4(rdram, ctx, runtime);
        uint32_t v_begin = *reinterpret_cast<const uint32_t *>(rdram + (a0 & 0x01FFFFFFu));
        uint32_t v_end = *reinterpret_cast<const uint32_t *>(rdram + ((a0 + 4) & 0x01FFFFFFu));
        uint32_t v_cap = *reinterpret_cast<const uint32_t *>(rdram + ((a0 + 8) & 0x01FFFFFFu));
        uint32_t count = (v_end >= v_begin) ? (v_end - v_begin) / 4 : 0;
        ps2xSafeLog("[func_1F28C4:EXIT] vector: begin=0x%08x end=0x%08x cap=0x%08x count=%u\n",
                    v_begin, v_end, v_cap, count);
        fflush(stderr);
    });

    const uint32_t slot360f00 = (0x00360F00u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig360f00 = g_ps2RecompiledFunctionTable[slot360f00];
    replaceFunction(0x00360F00u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t ra = getRegU32(ctx, 31);
        const char *s0 = (a0 && (a0 & 0x01FFFFFFu) < 0x02000000u) ? reinterpret_cast<const char *>(rdram + (a0 & 0x01FFFFFFu)) : "<null>";
        const char *s1 = (a1 && (a1 & 0x01FFFFFFu) < 0x02000000u) ? reinterpret_cast<const char *>(rdram + (a1 & 0x01FFFFFFu)) : "<null>";
        ps2xSafeLog("[sub_00360F00:ENTER] a0=0x%08x(\"%s\") a1=0x%08x(\"%s\") ra=0x%08x\n", a0, s0, a1, s1, ra);
        fflush(stderr);
        if (s_orig360f00) s_orig360f00(rdram, ctx, runtime);
        ps2xSafeLog("[sub_00360F00:EXIT] v0=0x%08x\n", getRegU32(ctx, 2));
        fflush(stderr);
    });

    const uint32_t slot35ab88 = (0x0035AB88u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig35ab88 = g_ps2RecompiledFunctionTable[slot35ab88];
    replaceFunction(0x0035AB88u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t a1 = getRegU32(ctx, 5);
        uint32_t ra = getRegU32(ctx, 31);
        const char *s1 = (a1 && (a1 & 0x01FFFFFFu) < 0x02000000u) ? reinterpret_cast<const char *>(rdram + (a1 & 0x01FFFFFFu)) : "<null>";
        if (s_orig35ab88) s_orig35ab88(rdram, ctx, runtime);
        if (a0 && (a0 & 0x01FFFFFFu) < 0x02000000u - 32) {
            const uint8_t *k = rdram + (a0 & 0x01FFFFFFu);
            ps2xSafeLog("[func_35AB88] str=\"%s\" (0x%08x) -> key=[%02x %02x %02x %02x %02x %02x %02x %02x ...] (ra=0x%08x)\n",
                        s1, a1, k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], ra);
            fflush(stderr);
        }
    });

    const uint32_t slot1bb508 = (0x001BB508u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig1bb508 = g_ps2RecompiledFunctionTable[slot1bb508];
    replaceFunction(0x001BB508u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4); // map
        uint32_t a1 = getRegU32(ctx, 5); // key
        uint32_t ra = getRegU32(ctx, 31);
        uint8_t k[32] = {};
        if (a1 && (a1 & 0x01FFFFFFu) < 0x02000000u - 32) {
            std::memcpy(k, rdram + (a1 & 0x01FFFFFFu), 32);
        }
        uint32_t inner = 0;
        if (a0 && (a0 & 0x01FFFFFFu) < 0x02000000u - 4) {
            std::memcpy(&inner, rdram + (a0 & 0x01FFFFFFu), 4);
        }
        if (s_orig1bb508) s_orig1bb508(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[func_1BB508] map=0x%08x (inner=0x%08x) key=[%02x %02x %02x %02x %02x %02x %02x %02x ...] (str=\"%s\") ra=0x%08x -> ret=0x%08x\n",
                    a0, inner, k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], (char*)k, ra, ret);
        uint32_t bucketArray = 0;
        if (inner && (inner & 0x01FFFFFFu) < 0x02000000u - 20) {
            std::memcpy(&bucketArray, rdram + ((inner + 16) & 0x01FFFFFFu), 4);
        }
        if (bucketArray && (bucketArray & 0x01FFFFFFu) < 0x02000000u - 512) {
            int totalNodes = 0;
            for (int b = 0; b < 128; ++b) {
                uint32_t node = 0;
                std::memcpy(&node, rdram + ((bucketArray + b * 4) & 0x01FFFFFFu), 4);
                while (node && (node & 0x01FFFFFFu) < 0x02000000u - 40) {
                    totalNodes++;
                    char nodeKey[33] = {};
                    std::memcpy(nodeKey, rdram + (node & 0x01FFFFFFu), 32);
                    uint32_t valPtr = 0, nextNode = 0;
                    std::memcpy(&valPtr, rdram + ((node + 0x20) & 0x01FFFFFFu), 4);
                    std::memcpy(&nextNode, rdram + ((node + 0x24) & 0x01FFFFFFu), 4);
                    if (std::strcmp(nodeKey, "p_3") == 0 || b < 3) {
                        ps2xSafeLog("  [bucket %d] node=0x%08x key=\"%s\" val=0x%08x next=0x%08x\n",
                                    b, node, nodeKey, valPtr, nextNode);
                    }
                    node = nextNode;
                }
            }
            ps2xSafeLog("[func_1BB508] total nodes in real map: %d (bucketArray=0x%08x)\n", totalNodes, bucketArray);
        }
        fflush(stderr);
    });

    const uint32_t slot2bb03c = (0x002BB03Cu - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig2bb03c = g_ps2RecompiledFunctionTable[slot2bb03c];
    replaceFunction(0x002BB03Cu, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t buf = getRegU32(ctx, 4);
        uint32_t sz = getRegU32(ctx, 5);
        uint32_t ra = getRegU32(ctx, 31);
        char preview[65] = {};
        if (buf && (buf & 0x01FFFFFFu) < 0x02000000u - 64) {
            for (int i = 0; i < 64; ++i) {
                uint8_t b = rdram[(buf & 0x01FFFFFFu) + i];
                preview[i] = (b >= 32 && b < 127) ? (char)b : '.';
            }
        }
        ps2xSafeLog("[sub_002BB03C:ENTER] buf=0x%08x size=%u ra=0x%08x preview=\"%.64s\"\n", buf, sz, ra, preview);
        fflush(stderr);
        if (s_orig2bb03c) s_orig2bb03c(rdram, ctx, runtime);
    });

    const uint32_t slot2ba584 = (0x002BA584u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig2ba584 = g_ps2RecompiledFunctionTable[slot2ba584];
    replaceFunction(0x002BA584u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t bufBefore = *reinterpret_cast<const uint32_t *>(rdram + (0x007406A8u & 0x01FFFFFFu));
        uint32_t szBefore = *reinterpret_cast<const uint32_t *>(rdram + (0x007406B0u & 0x01FFFFFFu));
        ps2xSafeLog("[sub_002BA584:ENTER] a0=0x%08x ra=0x%08x (buf=0x%08x sz=%u)\n", a0, ra, bufBefore, szBefore);
        fflush(stderr);
        if (s_orig2ba584) s_orig2ba584(rdram, ctx, runtime);
        uint32_t buf = *reinterpret_cast<const uint32_t *>(rdram + (0x007406A8u & 0x01FFFFFFu));
        uint32_t dataPtr = *reinterpret_cast<const uint32_t *>(rdram + (0x007406ACu & 0x01FFFFFFu));
        uint32_t linesStart = *reinterpret_cast<const uint32_t *>(rdram + (0x007406B4u & 0x01FFFFFFu));
        uint32_t linesEnd = *reinterpret_cast<const uint32_t *>(rdram + (0x007406B8u & 0x01FFFFFFu));
        uint32_t numFields = *reinterpret_cast<const uint32_t *>(rdram + (0x007406C4u & 0x01FFFFFFu));
        uint32_t numRecords = *reinterpret_cast<const uint32_t *>(rdram + (0x007406C8u & 0x01FFFFFFu));
        uint32_t recordSize = *reinterpret_cast<const uint32_t *>(rdram + (0x007406CCu & 0x01FFFFFFu));
        uint32_t nLines = (linesEnd >= linesStart) ? (linesEnd - linesStart) / 4 : 0;
        ps2xSafeLog("[sub_002BA584:EXIT] buf=0x%08x dataPtr=0x%08x lines=%u fields=%u records=%u recSize=%u\n",
                    buf, dataPtr, nLines, numFields, numRecords, recordSize);
        fflush(stderr);
    });

    const uint32_t slot2bb0d0 = (0x002BB0D0u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig2bb0d0 = g_ps2RecompiledFunctionTable[slot2bb0d0];
    replaceFunction(0x002BB0D0u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t ra = getRegU32(ctx, 31);
        uint32_t curVal = *reinterpret_cast<const uint32_t *>(rdram + (0x007406C8u & 0x01FFFFFFu));
        if (s_orig2bb0d0) s_orig2bb0d0(rdram, ctx, runtime);
        uint32_t ret = getRegU32(ctx, 2);
        ps2xSafeLog("[sub_002BB0D0] ra=0x%08x [0x7406C8]=%u -> v0=%u\n", ra, curVal, ret);
        fflush(stderr);
    });

    const uint32_t slot428c48 = (0x00428C48u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig428c48 = g_ps2RecompiledFunctionTable[slot428c48];
    replaceFunction(0x00428C48u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        uint32_t a0 = getRegU32(ctx, 4);
        uint32_t ra = getRegU32(ctx, 31);
        if (ra == 0x002BA6F8u || ra == 0x002BA6ACu || ra == 0x002BAA98u || ra == 0x002BAB48u) {
            const char *str = (a0 && (a0 & 0x01FFFFFFu) < 0x02000000u) ? reinterpret_cast<const char *>(rdram + (a0 & 0x01FFFFFFu)) : "<null>";
            if (s_orig428c48) s_orig428c48(rdram, ctx, runtime);
            uint32_t ret = getRegU32(ctx, 2);
            ps2xSafeLog("[atoi@0x%08x] str=\"%s\" (0x%08x) -> ret=%u\n", ra, str, a0, ret);
            fflush(stderr);
            return;
        }
        if (s_orig428c48) s_orig428c48(rdram, ctx, runtime);
    });
    }



    const uint32_t slot4369a8 = (0x004369A8u - g_ps2RecompiledFunctionTableBase) >> 2;
    static RecompiledFunction s_orig4369a8 = g_ps2RecompiledFunctionTable[slot4369a8];
    replaceFunction(0x004369A8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        const char *env = std::getenv("PS2X_TRACE_SYNC");
        if (env && std::strcmp(env, "1") == 0)
        {
            ps2xSafeLog("[sync:direct_iWakeupThread@0x4369A8] targetTid=%d ra=0x%08x\n",
                        (int32_t)getRegU32(ctx, 4), getRegU32(ctx, 31));
            fflush(stderr);
        }
        if (s_orig4369a8) s_orig4369a8(rdram, ctx, runtime);
    });

    static auto stub_sceSifGetSreg = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t val = 0;
        if (reg < 32)
        {
            if (const uint8_t *mem = getMemPtr(rdram, 0x0076A6C0u + reg * 4))
            {
                std::memcpy(&val, mem, 4);
            }
        }
        if (reg == 30 || reg == 31)
        {
            ps2xSafeLog( "[sreg] EE queried sreg[%u] -> 0x%x (ra=0x%08x)\n", reg, val, getRegU32(ctx, 31));
            fflush(stderr);
        }
        setReturnU32(ctx, val);
        ctx->pc = getRegU32(ctx, 31);
    };
    replaceFunction(0x004388F8u, stub_sceSifGetSreg);
    ps2xSafeLog( "[override] 0x004388F8 (sceSifGetSreg) registered with runtime stub\n");
    fflush(stderr);

    IsoDisc::instance().open();

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

namespace
{
    struct BoundConfigStub
    {
        std::string name;
        void (*handler)(uint8_t*, R5900Context*, PS2Runtime*);
    };

    static std::unordered_map<uint32_t, BoundConfigStub> s_boundConfigStubs;

    static const std::unordered_map<std::string, void (*)(uint8_t*, R5900Context*, PS2Runtime*)> &getAvailableRuntimeStubs()
    {
        static const std::unordered_map<std::string, void (*)(uint8_t*, R5900Context*, PS2Runtime*)> s_stubs = []{
            std::unordered_map<std::string, void (*)(uint8_t*, R5900Context*, PS2Runtime*)> m;
#define REG_STUB(name) m[#name] = &ps2_stubs::name;
            PS2_STUB_LIST(REG_STUB)
#undef REG_STUB
#define REG_SYSCALL(name) m[#name] = &ps2_syscalls::name;
            PS2_SYSCALL_LIST(REG_SYSCALL)
#undef REG_SYSCALL
            return m;
        }();
        return s_stubs;
    }

    static void genericConfigStubThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t ra = getRegU32(ctx, 31);
        ctx->pc = ra;
        auto it = s_boundConfigStubs.find(pc);
        if (it != s_boundConfigStubs.end())
        {
            static int s_tracePrintf = -1;
            if (s_tracePrintf == -1)
            {
                const char *env = std::getenv("PS2X_TRACE_PRINTF");
                s_tracePrintf = (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0)) ? 1 : 0;
            }

            if (s_tracePrintf == 0 && (it->second.name == "vfprintf" || it->second.name == "_vfprintf_r"))
            {
#ifdef _WIN32
                static int s_nullFd = -1;
                if (s_nullFd == -1) {
                    s_nullFd = _open("NUL", 0x0002 /* _O_RDWR */);
                }
                std::fflush(stderr);
                int saved_stderr = _dup(2);
                if (s_nullFd >= 0) {
                    _dup2(s_nullFd, 2);
                }
                it->second.handler(rdram, ctx, runtime);
                std::fflush(stderr);
                if (saved_stderr >= 0) {
                    _dup2(saved_stderr, 2);
                    _close(saved_stderr);
                }
#else
                it->second.handler(rdram, ctx, runtime);
#endif
            }
            else
            {
                it->second.handler(rdram, ctx, runtime);
                if (s_tracePrintf == 1 &&
                    (it->second.name == "vfprintf" || it->second.name == "_vfprintf_r" ||
                     it->second.name == "sprintf" || it->second.name == "_sprintf_r" ||
                     it->second.name == "printf" || it->second.name == "_printf_r"))
                {
                    std::lock_guard<std::mutex> lock(s_logMutex);
                    std::fputc('\n', stderr);
                    std::fflush(stderr);
                }
            }
        }
        else
        {
            ps2xSafeLog( "[stub] genericConfigStubThunk: unbound stub at pc=0x%08x\n", pc);
            fflush(stderr);
        }
        if (ctx->pc == pc)
        {
            ctx->pc = ra;
        }
    }

    static std::unordered_map<uint32_t, std::string> s_unimplementedConfigStubs;

    static void unimplementedConfigStubThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t ra = getRegU32(ctx, 31);
        auto it = s_unimplementedConfigStubs.find(pc);
        const char *name = (it != s_unimplementedConfigStubs.end()) ? it->second.c_str() : "unknown";
        ps2xSafeLog( "\n[FATAL] Called unimplemented config stub '%s' at 0x%08x (ra=0x%08x)!\n", name, pc, ra);
        fflush(stderr);
        std::abort();
    }
}

static void registerConfigStubs(PS2Runtime &runtime, const std::string &configPath)
{
    std::ifstream file;
    std::string resolvedPath;

    std::vector<std::string> candidates;
    candidates.push_back(configPath);
    candidates.push_back("../" + configPath);
    candidates.push_back("../../" + configPath);

#ifdef _WIN32
    char exeBuf[1024] = {0};
    if (GetModuleFileNameA(NULL, exeBuf, static_cast<unsigned long>(sizeof(exeBuf))) > 0)
    {
        std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        candidates.push_back((exeDir / configPath).string());
        candidates.push_back((exeDir / ".." / configPath).string());
        candidates.push_back((exeDir / ".." / ".." / configPath).string());
        candidates.push_back((exeDir / ".." / ".." / ".." / configPath).string());
    }
#endif
    candidates.push_back("c:/ApeEscape3_Recomp/" + configPath);

    for (const auto &cand : candidates)
    {
        file.open(cand);
        if (file.is_open())
        {
            resolvedPath = cand;
            break;
        }
    }

    if (!file.is_open())
    {
        ps2xSafeLog( "\n[config-stubs] FATAL: Could not open %s in any searched location!\n", configPath.c_str());
        fflush(stderr);
        std::abort();
    }

    const auto &available = getAvailableRuntimeStubs();
    size_t matchedCount = 0;
    size_t unmatchedCount = 0;

    std::string line;
    while (std::getline(file, line))
    {
        const size_t atPos = line.find("@0x");
        if (atPos == std::string::npos)
        {
            continue;
        }

        size_t startPos = line.find_first_not_of(" \t\"'");
        if (startPos == std::string::npos || startPos >= atPos)
        {
            continue;
        }
        std::string name = line.substr(startPos, atPos - startPos);
        while (!name.empty() && (name.back() == '\"' || name.back() == '\'' || name.back() == ' ' || name.back() == '\t'))
        {
            name.pop_back();
        }

        std::string addrStr = line.substr(atPos + 3);
        size_t endHex = addrStr.find_first_not_of("0123456789abcdefABCDEF");
        if (endHex != std::string::npos)
        {
            addrStr = addrStr.substr(0, endHex);
        }
        if (addrStr.empty())
        {
            continue;
        }

        uint32_t addr = 0;
        try {
            addr = static_cast<uint32_t>(std::stoul(addrStr, nullptr, 16));
        } catch (...) {
            continue;
        }

        auto it = available.find(name);
        if (it != available.end())
        {
            s_boundConfigStubs[addr] = {name, it->second};
            runtime.replaceFunction(addr, genericConfigStubThunk);
            ++matchedCount;
        }
        else
        {
            ++unmatchedCount;
            if (!runtime.hasFunction(addr))
            {
                s_unimplementedConfigStubs[addr] = name;
                runtime.replaceFunction(addr, unimplementedConfigStubThunk);
            }
        }
    }

    if (matchedCount == 0)
    {
        ps2xSafeLog( "\n[config-stubs] FATAL: 0 stubs matched from %s!\n", resolvedPath.c_str());
        fflush(stderr);
        std::abort();
    }

    ps2xSafeLog( "[config-stubs] Registered %zu stubs from %s into function table (%zu stubs unmatched in runtime)\n",
            matchedCount, resolvedPath.c_str(), unmatchedCount);
    fflush(stderr);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

static bool executeUnrecompiledCode(uint8_t *rdram,
                                    R5900Context *ctx,
                                    uint32_t targetPc,
                                    uint32_t sourcePc,
                                    uint32_t fallthroughPc,
                                    PS2Runtime::GuestBranchKind kind,
                                    const char *debugName,
                                    PS2Runtime *runtime)
{
    const bool isCall = (kind == PS2Runtime::GuestBranchKind::DirectCall || kind == PS2Runtime::GuestBranchKind::IndirectCall);
    uint32_t pc = targetPc;
    const uint32_t initialRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));

    static uint32_t s_interpCount = 0;
    if (++s_interpCount <= 20 || (s_interpCount % 1000) == 0)
    {
        ps2xSafeLog("[guest-fallback-interp] entry #%u targetPc=0x%08x from sourcePc=0x%08x kind=%s ra=0x%08x\n",
                    s_interpCount, targetPc, sourcePc, describeGuestBranchKind(kind), initialRa);
        fflush(stderr);
    }

    auto readGPR = [ctx](int r) -> uint64_t {
        if (r == 0) return 0;
        return static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[r]));
    };
    auto readGPR32 = [ctx](int r) -> uint32_t {
        if (r == 0) return 0;
        return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0));
    };
    auto writeGPR32 = [ctx](int r, uint32_t val) {
        if (r != 0) {
            ctx->r[r] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(val)));
        }
    };
    auto writeGPR64 = [ctx](int r, uint64_t val) {
        if (r != 0) {
            ctx->r[r] = _mm_set_epi64x(0, static_cast<int64_t>(val));
        }
    };

    auto read8 = [rdram](uint32_t a) -> uint8_t {
        return *reinterpret_cast<const uint8_t *>(rdram + (a & PS2_RAM_MASK));
    };
    auto read16 = [rdram](uint32_t a) -> uint16_t {
        return *reinterpret_cast<const uint16_t *>(rdram + (a & PS2_RAM_MASK));
    };
    auto read32 = [rdram](uint32_t a) -> uint32_t {
        return *reinterpret_cast<const uint32_t *>(rdram + (a & PS2_RAM_MASK));
    };
    auto read64 = [rdram](uint32_t a) -> uint64_t {
        return *reinterpret_cast<const uint64_t *>(rdram + (a & PS2_RAM_MASK));
    };
    auto write8 = [rdram](uint32_t a, uint8_t v) {
        *reinterpret_cast<uint8_t *>(rdram + (a & PS2_RAM_MASK)) = v;
    };
    auto write16 = [rdram](uint32_t a, uint16_t v) {
        *reinterpret_cast<uint16_t *>(rdram + (a & PS2_RAM_MASK)) = v;
    };
    auto write32 = [rdram](uint32_t a, uint32_t v) {
        *reinterpret_cast<uint32_t *>(rdram + (a & PS2_RAM_MASK)) = v;
    };
    auto write64 = [rdram](uint32_t a, uint64_t v) {
        *reinterpret_cast<uint64_t *>(rdram + (a & PS2_RAM_MASK)) = v;
    };

    uint32_t stepLimit = 50000;
    while (stepLimit-- > 0 && !runtime->isStopRequested())
    {
        if (pc != targetPc && runtime->hasFunction(pc))
        {
            ctx->pc = pc;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(pc);
            fn(rdram, ctx, runtime);
            if (isCall && (ctx->pc == initialRa || ctx->pc == fallthroughPc || ctx->pc == 0))
            {
                return true;
            }
            pc = ctx->pc;
            if (pc == 0 || pc == initialRa || pc == fallthroughPc)
            {
                break;
            }
            continue;
        }

        if (pc < 0x00100000u || pc >= PS2_RAM_SIZE)
        {
            ps2xSafeLog("[guest-fallback-interp] PC out of range: 0x%08x\n", pc);
            break;
        }

        uint32_t inst = read32(pc);
        uint32_t op = inst >> 26;
        uint32_t rs = (inst >> 21) & 0x1F;
        uint32_t rt = (inst >> 16) & 0x1F;
        uint32_t rd = (inst >> 11) & 0x1F;
        uint32_t shamt = (inst >> 6) & 0x1F;
        uint32_t funct = inst & 0x3F;
        int16_t imm16 = static_cast<int16_t>(inst & 0xFFFF);
        uint16_t uimm16 = static_cast<uint16_t>(inst & 0xFFFF);
        uint32_t signImm32 = static_cast<uint32_t>(static_cast<int32_t>(imm16));

        auto execInst = [&](uint32_t i_pc, uint32_t i_inst) {
            uint32_t i_op = i_inst >> 26;
            uint32_t i_rs = (i_inst >> 21) & 0x1F;
            uint32_t i_rt = (i_inst >> 16) & 0x1F;
            uint32_t i_rd = (i_inst >> 11) & 0x1F;
            uint32_t i_shamt = (i_inst >> 6) & 0x1F;
            uint32_t i_funct = i_inst & 0x3F;
            int16_t i_imm16 = static_cast<int16_t>(i_inst & 0xFFFF);
            uint16_t i_uimm16 = static_cast<uint16_t>(i_inst & 0xFFFF);
            uint32_t i_signImm32 = static_cast<uint32_t>(static_cast<int32_t>(i_imm16));

            if (i_op == 0) // SPECIAL
            {
                switch (i_funct)
                {
                case 0x00: writeGPR32(i_rd, readGPR32(i_rt) << i_shamt); break; // SLL
                case 0x02: writeGPR32(i_rd, readGPR32(i_rt) >> i_shamt); break; // SRL
                case 0x03: writeGPR32(i_rd, static_cast<int32_t>(readGPR32(i_rt)) >> i_shamt); break; // SRA
                case 0x04: writeGPR32(i_rd, readGPR32(i_rt) << (readGPR32(i_rs) & 0x1F)); break; // SLLV
                case 0x06: writeGPR32(i_rd, readGPR32(i_rt) >> (readGPR32(i_rs) & 0x1F)); break; // SRLV
                case 0x07: writeGPR32(i_rd, static_cast<int32_t>(readGPR32(i_rt)) >> (readGPR32(i_rs) & 0x1F)); break; // SRAV
                case 0x0A: if (readGPR32(i_rt) == 0) writeGPR64(i_rd, readGPR(i_rs)); break; // MOVZ
                case 0x0B: if (readGPR32(i_rt) != 0) writeGPR64(i_rd, readGPR(i_rs)); break; // MOVN
                case 0x0F: break; // SYNC
                case 0x14: writeGPR64(i_rd, readGPR(i_rt) << (readGPR32(i_rs) & 0x3F)); break; // DSLLV
                case 0x16: writeGPR64(i_rd, readGPR(i_rt) >> (readGPR32(i_rs) & 0x3F)); break; // DSRLV
                case 0x17: writeGPR64(i_rd, static_cast<int64_t>(readGPR(i_rt)) >> (readGPR32(i_rs) & 0x3F)); break; // DSRAV
                case 0x20: // ADD
                case 0x21: writeGPR32(i_rd, readGPR32(i_rs) + readGPR32(i_rt)); break; // ADDU
                case 0x22: // SUB
                case 0x23: writeGPR32(i_rd, readGPR32(i_rs) - readGPR32(i_rt)); break; // SUBU
                case 0x24: writeGPR64(i_rd, readGPR(i_rs) & readGPR(i_rt)); break; // AND
                case 0x25: writeGPR64(i_rd, readGPR(i_rs) | readGPR(i_rt)); break; // OR
                case 0x26: writeGPR64(i_rd, readGPR(i_rs) ^ readGPR(i_rt)); break; // XOR
                case 0x27: writeGPR64(i_rd, ~(readGPR(i_rs) | readGPR(i_rt))); break; // NOR
                case 0x2A: writeGPR32(i_rd, (static_cast<int32_t>(readGPR32(i_rs)) < static_cast<int32_t>(readGPR32(i_rt))) ? 1 : 0); break; // SLT
                case 0x2B: writeGPR32(i_rd, (readGPR32(i_rs) < readGPR32(i_rt)) ? 1 : 0); break; // SLTU
                case 0x2C: // DADD
                case 0x2D: writeGPR64(i_rd, readGPR(i_rs) + readGPR(i_rt)); break; // DADDU
                case 0x2E: // DSUB
                case 0x2F: writeGPR64(i_rd, readGPR(i_rs) - readGPR(i_rt)); break; // DSUBU
                case 0x38: writeGPR64(i_rd, readGPR(i_rt) << i_shamt); break; // DSLL
                case 0x3A: writeGPR64(i_rd, readGPR(i_rt) >> i_shamt); break; // DSRL
                case 0x3B: writeGPR64(i_rd, static_cast<int64_t>(readGPR(i_rt)) >> i_shamt); break; // DSRA
                case 0x3C: writeGPR64(i_rd, readGPR(i_rt) << (i_shamt + 32)); break; // DSLL32
                case 0x3E: writeGPR64(i_rd, readGPR(i_rt) >> (i_shamt + 32)); break; // DSRL32
                case 0x3F: writeGPR64(i_rd, static_cast<int64_t>(readGPR(i_rt)) >> (i_shamt + 32)); break; // DSRA32
                default: break;
                }
            }
            else
            {
                switch (i_op)
                {
                case 0x08: // ADDI
                case 0x09: writeGPR32(i_rt, readGPR32(i_rs) + i_signImm32); break; // ADDIU
                case 0x0A: writeGPR32(i_rt, (static_cast<int32_t>(readGPR32(i_rs)) < static_cast<int32_t>(i_signImm32)) ? 1 : 0); break; // SLTI
                case 0x0B: writeGPR32(i_rt, (readGPR32(i_rs) < i_signImm32) ? 1 : 0); break; // SLTIU
                case 0x0C: writeGPR64(i_rt, readGPR(i_rs) & static_cast<uint64_t>(i_uimm16)); break; // ANDI
                case 0x0D: writeGPR64(i_rt, readGPR(i_rs) | static_cast<uint64_t>(i_uimm16)); break; // ORI
                case 0x0E: writeGPR64(i_rt, readGPR(i_rs) ^ static_cast<uint64_t>(i_uimm16)); break; // XORI
                case 0x0F: writeGPR32(i_rt, static_cast<uint32_t>(i_uimm16) << 16); break; // LUI
                case 0x19: writeGPR64(i_rt, readGPR(i_rs) + static_cast<int64_t>(i_imm16)); break; // DADDIU
                case 0x20: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR32(i_rt, static_cast<int8_t>(read8(ea))); break; } // LB
                case 0x21: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR32(i_rt, static_cast<int16_t>(read16(ea))); break; } // LH
                case 0x23: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR32(i_rt, read32(ea)); break; } // LW
                case 0x24: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR32(i_rt, read8(ea)); break; } // LBU
                case 0x25: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR32(i_rt, read16(ea)); break; } // LHU
                case 0x27: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR64(i_rt, read32(ea)); break; } // LWU
                case 0x37: { uint32_t ea = readGPR32(i_rs) + i_signImm32; writeGPR64(i_rt, read64(ea)); break; } // LD
                case 0x28: { uint32_t ea = readGPR32(i_rs) + i_signImm32; write8(ea, static_cast<uint8_t>(readGPR32(i_rt))); break; } // SB
                case 0x29: { uint32_t ea = readGPR32(i_rs) + i_signImm32; write16(ea, static_cast<uint16_t>(readGPR32(i_rt))); break; } // SH
                case 0x2B: { uint32_t ea = readGPR32(i_rs) + i_signImm32; write32(ea, readGPR32(i_rt)); break; } // SW
                case 0x3F: { uint32_t ea = readGPR32(i_rs) + i_signImm32; write64(ea, readGPR(i_rt)); break; } // SD
                default: break;
                }
            }
        };

        if (op == 0 && funct == 0x08) // JR rs
        {
            uint32_t jumpTarget = readGPR32(rs);
            execInst(pc + 4, read32(pc + 4));
            ctx->pc = jumpTarget;
            if (isCall && (jumpTarget == initialRa || jumpTarget == fallthroughPc || rs == 31))
            {
                return true;
            }
            pc = jumpTarget;
            continue;
        }
        else if (op == 0 && funct == 0x09) // JALR rd, rs
        {
            uint32_t callTarget = readGPR32(rs);
            uint32_t returnAddr = pc + 8;
            writeGPR32(rd, returnAddr);
            execInst(pc + 4, read32(pc + 4));
            if (runtime->hasFunction(callTarget))
            {
                ctx->pc = callTarget;
                PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(callTarget);
                fn(rdram, ctx, runtime);
                pc = returnAddr;
            }
            else
            {
                pc = callTarget;
            }
            continue;
        }
        else if (op == 0x02) // J target
        {
            uint32_t jumpTarget = (pc & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
            execInst(pc + 4, read32(pc + 4));
            pc = jumpTarget;
            continue;
        }
        else if (op == 0x03) // JAL target
        {
            uint32_t callTarget = (pc & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
            uint32_t returnAddr = pc + 8;
            writeGPR32(31, returnAddr);
            execInst(pc + 4, read32(pc + 4));
            if (runtime->hasFunction(callTarget))
            {
                ctx->pc = callTarget;
                PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(callTarget);
                fn(rdram, ctx, runtime);
                pc = returnAddr;
            }
            else
            {
                pc = callTarget;
            }
            continue;
        }
        else if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 || op == 0x01) // Branches
        {
            bool taken = false;
            if (op == 0x04) taken = (readGPR(rs) == readGPR(rt)); // BEQ
            else if (op == 0x05) taken = (readGPR(rs) != readGPR(rt)); // BNE
            else if (op == 0x06) taken = (static_cast<int64_t>(readGPR(rs)) <= 0); // BLEZ
            else if (op == 0x07) taken = (static_cast<int64_t>(readGPR(rs)) > 0); // BGTZ
            else if (op == 0x01) { // REGIMM
                if (rt == 0x00) taken = (static_cast<int64_t>(readGPR(rs)) < 0); // BLTZ
                else if (rt == 0x01) taken = (static_cast<int64_t>(readGPR(rs)) >= 0); // BGEZ
            }
            uint32_t branchTarget = pc + 4 + (static_cast<int32_t>(imm16) << 2);
            execInst(pc + 4, read32(pc + 4)); // delay slot
            pc = taken ? branchTarget : (pc + 8);
            continue;
        }
        else
        {
            execInst(pc, inst);
            pc += 4;
        }
    }

    ctx->pc = pc;
    return true;
}

struct EeProfilerScope {
    static inline thread_local int s_depth = 0;
    static inline thread_local std::chrono::steady_clock::time_point s_start{};
    EeProfilerScope() {
        if (s_depth++ == 0) {
            s_start = std::chrono::steady_clock::now();
        }
    }
    ~EeProfilerScope() {
        if (--s_depth == 0) {
            auto end = std::chrono::steady_clock::now();
            g_profEeUs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - s_start).count()), std::memory_order_relaxed);
        }
    }
};

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    EeProfilerScope eeScope;
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
    if (targetPc >= 0x00120000u && targetPc < 0x0013F800u)
    {
        static uint32_t s_range120kBranchHitCount = 0;
        s_range120kBranchHitCount++;
        static bool s_reported120kBranch = false;
        if (!s_reported120kBranch)
        {
            s_reported120kBranch = true;
            ps2xSafeLog( "[region-0x120000:branch] First branch into 0x120000..0x13F800 at targetPc=0x%08x from sourcePc=0x%08x (total hits: %u)\n",
                    targetPc, sourcePc, s_range120kBranchHitCount);
            fflush(stderr);
        }
        if (ps2_stubs::g_hasReadInto120000.load(std::memory_order_acquire))
        {
            static uint32_t s_branchPostReadCount = 0;
            s_branchPostReadCount++;
            static bool s_reportedBranchPostRead = false;
            if (!s_reportedBranchPostRead)
            {
                s_reportedBranchPostRead = true;
                ps2xSafeLog( "[region-0x120000:post-read-branch] First branch into 0x120000..0x13F800 post-sceCdRead at targetPc=0x%08x from sourcePc=0x%08x (count=%u)\n",
                        targetPc, sourcePc, s_branchPostReadCount);
                fflush(stderr);
            }
        }
    }

    // Every inter-function transfer accounts cycles.
    // Never return false here for calls/transfers, as that unwinds host C++ frames and breaks main loop.
    if (m_eeScheduler)
    {
        m_eeScheduler->accountCycles(EeScheduler::kGuestDispatchCycles);
    }

    if (!isCall)
    {
        if ((kind == GuestBranchKind::DirectJump || kind == GuestBranchKind::IndirectJump) && hasFunction(targetPc))
        {
            RecompiledFunction targetFn = lookupFunction(targetPc);
            ctx->pc = targetPc;
            targetFn(rdram, ctx, this);
            return false;
        }

        if (!hasFunction(targetPc))
        {
            if (m_memory.isCodeAddress(targetPc) || (targetPc >= 0x00100000u && targetPc < 0x00628248u))
            {
                return executeUnrecompiledCode(rdram, ctx, targetPc, sourcePc, 0u, kind, debugName, this);
            }
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        if (m_memory.isCodeAddress(targetPc) || (targetPc >= 0x00100000u && targetPc < 0x00628248u))
        {
            return executeUnrecompiledCode(rdram, ctx, targetPc, sourcePc, fallthroughPc, kind, debugName, this);
        }

        ps2xSafeLog( "[abort] dispatchGuestBranch: missing target at targetPc=0x%08x, sourcePc=0x%08x, kind=%s, op=%s, ra=0x%08x, sp=0x%08x\n",
                targetPc, sourcePc, describeGuestBranchKind(kind), (debugName ? debugName : "<unknown>"),
                (uint32_t)_mm_extract_epi32(ctx->r[31], 0),
                (uint32_t)_mm_extract_epi32(ctx->r[29], 0));
        fflush(stderr);
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        std::abort();
    }

    extern thread_local uint32_t g_currentExecutingFunction;
    const uint32_t prevExecutingFn = g_currentExecutingFunction;
    g_currentExecutingFunction = targetPc;

    // Track for movie/MPEG calls:
    if (targetPc == 0x0031A1B4u || targetPc == 0x0031A240u ||
        (targetPc >= 0x00408000u && targetPc <= 0x00413000u))
    {
        g_mpegFunctionCalled.store(true, std::memory_order_relaxed);
    }
    // Track last 5 distinct function entry addresses:
    if (targetPc != g_lastDistinctFns[0])
    {
        g_lastDistinctFns[4] = g_lastDistinctFns[3];
        g_lastDistinctFns[3] = g_lastDistinctFns[2];
        g_lastDistinctFns[2] = g_lastDistinctFns[1];
        g_lastDistinctFns[1] = g_lastDistinctFns[0];
        g_lastDistinctFns[0] = targetPc;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    targetFn(rdram, ctx, this);
    g_currentExecutingFunction = prevExecutingFn;

    if (targetPc == 0x001037E8u)
    {
        while (!isStopRequested() && ctx->pc != 0u && hasFunction(ctx->pc))
        {
            const uint32_t currentPc = ctx->pc;
            RecompiledFunction fn = lookupFunction(currentPc);
            fn(rdram, ctx, this);
            if (ctx->pc == currentPc)
            {
                break;
            }
        }
        return true;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    // If the target function fell through or tail-jumped to another function,
    // continue dispatching chained functions until it returns to fallthroughPc.
    uint32_t dispatchChainSteps = 0;
    while (ctx->pc != fallthroughPc && !isStopRequested() && ctx->pc != 0u && hasFunction(ctx->pc))
    {
        if (++dispatchChainSteps > 1000)
        {
            // Safety bound against runaway dispatch chains
            break;
        }
        if (m_eeScheduler)
        {
            m_eeScheduler->accountCycles(EeScheduler::kGuestDispatchCycles);
        }
        const uint32_t nextPc = ctx->pc;
        RecompiledFunction nextFn = lookupFunction(nextPc);
        nextFn(rdram, ctx, this);
        if (ctx->pc == nextPc)
        {
            break;
        }
    }

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }


    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    static int s_traceSync = -1;
    if (s_traceSync == -1)
    {
        const char *env = std::getenv("PS2X_TRACE_SYNC");
        s_traceSync = (env && std::strcmp(env, "1") == 0) ? 1 : 0;
    }
    if (s_traceSync == 1)
    {
        if (syscallId == 0x20u)
        {
            uint32_t param = getRegU32(ctx, 4);
            uint32_t entry = *reinterpret_cast<const uint32_t *>(rdram + ((param + 4) & 0x01FFFFFFu));
            uint32_t stack = *reinterpret_cast<const uint32_t *>(rdram + ((param + 8) & 0x01FFFFFFu));
            uint32_t stackSize = *reinterpret_cast<const uint32_t *>(rdram + ((param + 12) & 0x01FFFFFFu));
            uint32_t prio = *reinterpret_cast<const uint32_t *>(rdram + ((param + 20) & 0x01FFFFFFu));
            ps2xSafeLog("[sync:CreateThread] param=0x%08x entry=0x%08x stack=0x%08x size=0x%x prio=%u ra=0x%08x\n",
                        param, entry, stack, stackSize, prio, getRegU32(ctx, 31));
            fflush(stderr);
        }
        else if (syscallId == 0x32u)
        {
            static uint32_t s_sleepCount = 0;
            if (s_sleepCount++ < 30 || (s_sleepCount % 500) == 0)
            {
                ps2xSafeLog("[sync:SleepThread #%u] pc=0x%08x ra=0x%08x sp=0x%08x\n",
                            s_sleepCount, ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 29));
                fflush(stderr);
            }
        }
        else if (syscallId == 0x33u)
        {
            ps2xSafeLog("[sync:WakeupThread] targetTid=%d ra=0x%08x\n",
                        (int32_t)getRegU32(ctx, 4), getRegU32(ctx, 31));
            fflush(stderr);
        }
        else if (syscallId == (uint32_t)-52 || syscallId == 0xFFFFFFCCu)
        {
            ps2xSafeLog("[sync:iWakeupThread] targetTid=%d ra=0x%08x\n",
                        (int32_t)getRegU32(ctx, 4), getRegU32(ctx, 31));
            fflush(stderr);
        }
    }

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

extern std::vector<uint32_t> consumeCompletedIntcCauses();

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
    for (uint32_t cause : consumeCompletedIntcCauses())
    {
        m_eeScheduler->dispatchIrq(false, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    constexpr uint32_t kMainThreadStackBottom = 0x01FA0000u;
    if (top > kMainThreadStackBottom)
    {
        top = kMainThreadStackBottom;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    const uint32_t stackPointer = top - 0x10u;
    const uint32_t poolRemaining = (base >= m_asyncCallbackStackFloor) ? (base - m_asyncCallbackStackFloor) : 0u;
    ps2xSafeLog( "[reserveAsyncCallbackStack] Reserved async stack [0x%08x..0x%08x], sp=0x%08x (floor=0x%08x, poolRemaining=%u bytes)\n",
                 base, top, stackPointer, m_asyncCallbackStackFloor, poolRemaining);
    std::fflush(stderr);
    return stackPointer;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler ? m_eeScheduler->checkpointDue(cycles) : false;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

#if AGRESSIVE_LOGS
    if (!std::getenv("PS2X_ENABLE_CALL_TRACE"))
    {
        ps2_log::log_stream().setstate(std::ios_base::badbit | std::ios_base::failbit);
    }
#endif

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    extern void dumpEeDiagnostics(const EeScheduler *scheduler, uint32_t stuckPc);
    uint64_t tick = 0;
    if (isVerboseLog()) {
        fprintf(stderr, "=== MAIN THREAD RUN LOOP STARTING ===\n");
        fflush(stderr);
    }
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        tick++;
        if (isVerboseLog() && (tick <= 5 || (tick % 60) == 0))
        {
            fprintf(stderr, "=== TICK %llu ===\n", (unsigned long long)tick);
            fflush(stderr);
        }

        if (isVerboseLog() && ((tick % 60) == 0))
        {
            extern std::string ps2xGetIntcSummary();
            ps2xSafeLog("[run:sec] tick=%llu dma=%llu gif=%llu vif=%llu intc:[%s]\n",
                        (unsigned long long)tick,
                        (unsigned long long)m_memory.dmaStartCount(),
                        (unsigned long long)m_memory.gifCopyCount(),
                        (unsigned long long)m_memory.vifWriteCount(),
                        ps2xGetIntcSummary().c_str());
        }

        if (tick == 300 || tick == 900 || tick == 1500)
        {
            extern void dumpEeDiagnostics(const EeScheduler *scheduler, uint32_t stuckPc);
            const uint32_t curPc = m_debugPc.load(std::memory_order_relaxed);
            dumpEeDiagnostics(m_eeScheduler.get(), curPc);
        }
        if (isVerboseLog() && ((tick % 120) == 0))
        {
            const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
            const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
            const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
            const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
            const uint64_t curDma = m_memory.dmaStartCount();
            const uint64_t curGif = m_memory.gifCopyCount();
            const uint64_t curGs = m_memory.gsWriteCount();
            const uint64_t curVif = m_memory.vifWriteCount();
            const auto eeSnapshot = m_eeScheduler->snapshot();
            ps2xSafeLog("[run:tick] tick=%llu pc=0x%08x ra=0x%08x sp=0x%08x gp=0x%08x activeThreads=%zu dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                        (unsigned long long)tick, dbgPc, dbgRa, dbgSp, dbgGp, eeSnapshot.threads.size(),
                        (unsigned long long)curDma, (unsigned long long)curGif, (unsigned long long)curGs, (unsigned long long)curVif);

            static std::vector<uint32_t> s_watchAddrs;
            static bool s_watchParsed = false;
            if (!s_watchParsed)
            {
                s_watchParsed = true;
                if (const char *envWatch = std::getenv("PS2X_WATCH"))
                {
                    std::string envStr(envWatch);
                    size_t start = 0;
                    while (start < envStr.size())
                    {
                        size_t comma = envStr.find(',', start);
                        std::string token = envStr.substr(start, (comma == std::string::npos) ? std::string::npos : (comma - start));
                        if (!token.empty())
                        {
                            try {
                                s_watchAddrs.push_back(std::stoul(token, nullptr, 0));
                            } catch (...) {}
                        }
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
            }
            if (!s_watchAddrs.empty())
            {
                uint8_t *rdram = m_memory.getRDRAM();
                std::string watchLine = "[run:watch]";
                char buf[64];
                for (uint32_t addr : s_watchAddrs)
                {
                    if (addr + 4 <= PS2_RAM_SIZE)
                    {
                        uint32_t val32 = *reinterpret_cast<const uint32_t *>(rdram + addr);
                        uint8_t val8 = *reinterpret_cast<const uint8_t *>(rdram + addr);
                        std::snprintf(buf, sizeof(buf), " 0x%08x=0x%08x(u8=0x%02x)", addr, val32, val8);
                        watchLine += buf;
                    }
                }
                watchLine += "\n";
                ps2xSafeLog("%s", watchLine.c_str());
            }


            static uint32_t s_lastPc = 0;
            static int s_consecutiveSamePc = 0;
            static bool s_dumpDone = false;
            const bool inStuckLoop = (dbgPc >= 0x0024FA40u && dbgPc <= 0x0024FA60u) || (dbgPc == 0x0035f4b4u);
            if (inStuckLoop)
            {
                s_consecutiveSamePc++;
                if (s_consecutiveSamePc >= 5 && !s_dumpDone)
                {
                    s_dumpDone = true;
                    if (const char *dumpEnv = std::getenv("PS2X_THREAD_DUMP"))
                    {
                        if (std::strcmp(dumpEnv, "1") == 0)
                        {
                            dumpEeDiagnostics(m_eeScheduler.get(), dbgPc);

                            const GuestThread *th1 = m_eeScheduler->thread(1);
                            if (th1)
                            {
                                const R5900Context &c1 = th1->activeContext();
                                uint32_t s7 = getRegU32(&c1, 23);
                                uint32_t s3 = getRegU32(&c1, 19);
                                uint32_t t6 = getRegU32(&c1, 14);
                                uint32_t t7 = getRegU32(&c1, 15);
                                uint32_t a0 = getRegU32(&c1, 4);
                                uint32_t a1 = getRegU32(&c1, 5);
                                uint32_t v0 = getRegU32(&c1, 2);
                                uint8_t *rdram = m_memory.getRDRAM();
                                std::string stallMsg;
                                char sbuf[256];
                                std::snprintf(sbuf, sizeof(sbuf), "\n[STALL_DIAG] Thread 1 regs: s7=0x%08x s3=0x%08x t6=0x%08x t7=0x%08x a0=0x%08x a1=0x%08x v0=0x%08x\n",
                                             s7, s3, t6, t7, a0, a1, v0);
                                stallMsg += sbuf;
                                if (s7 != 0 && s7 + 0x30 <= PS2_RAM_SIZE)
                                {
                                    uint32_t iterCur = *reinterpret_cast<const uint32_t *>(rdram + s7 + 0x1C);
                                    uint32_t iterEnd = *reinterpret_cast<const uint32_t *>(rdram + s7 + 0x2C);
                                    std::snprintf(sbuf, sizeof(sbuf), "[STALL_DIAG] Container at 0x%08x: iterCur=0x%08x iterEnd=0x%08x (diff=%d)\n",
                                                 s7, iterCur, iterEnd, static_cast<int>(iterEnd - iterCur));
                                    stallMsg += sbuf;
                                }
                                if (s3 != 0 && s3 + 0x64 <= PS2_RAM_SIZE)
                                {
                                    uint32_t taskId = *reinterpret_cast<const uint32_t *>(rdram + s3 + 0x14);
                                    uint32_t state = *reinterpret_cast<const uint32_t *>(rdram + s3 + 0x18);
                                    uint32_t ptr1c = *reinterpret_cast<const uint32_t *>(rdram + s3 + 0x1C);
                                    uint32_t err5c = *reinterpret_cast<const uint32_t *>(rdram + s3 + 0x5C);
                                    std::snprintf(sbuf, sizeof(sbuf), "[STALL_DIAG] Element at 0x%08x: taskId=0x%08x state=%u ptr1c=0x%08x err5c=0x%08x\n",
                                                 s3, taskId, state, ptr1c, err5c);
                                    stallMsg += sbuf;
                                    if (ptr1c != 0 && ptr1c < PS2_RAM_SIZE)
                                    {
                                        const char *str = reinterpret_cast<const char *>(rdram + ptr1c);
                                        std::snprintf(sbuf, sizeof(sbuf), "[STALL_DIAG] Element string at ptr1c: '%.64s'\n", str);
                                        stallMsg += sbuf;
                                    }
                                }
                                constexpr uint32_t kFile3Base = 0x00771798u;
                                if (kFile3Base + 0x40 <= PS2_RAM_SIZE)
                                {
                                    std::snprintf(sbuf, sizeof(sbuf), "[STALL_DIAG] file3 at 0x%08x:\n", kFile3Base);
                                    stallMsg += sbuf;
                                    for (uint32_t i = 0; i < 32; i += 4)
                                    {
                                        uint32_t val = *reinterpret_cast<const uint32_t *>(rdram + kFile3Base + i);
                                        std::snprintf(sbuf, sizeof(sbuf), "  +0x%02x: 0x%08x\n", i, val);
                                        stallMsg += sbuf;
                                    }
                                }
                                ps2xSafeLog("%s", stallMsg.c_str());
                            }
                        }
                    }
                }
            }
            else
            {
                s_lastPc = dbgPc;
                s_consecutiveSamePc = 1;
            }
        }
        // Sample real keyboard state from Raylib window thread
        uint16_t activeButtons = 0;
        if (IsKeyDown(KEY_ENTER)) activeButtons |= (1u << 3); // START
        if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_BACKSPACE)) activeButtons |= (1u << 0); // SELECT
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_K)) activeButtons |= (1u << 14); // CROSS
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_L) || IsKeyDown(KEY_SPACE)) activeButtons |= (1u << 13); // CIRCLE
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_J)) activeButtons |= (1u << 15); // SQUARE
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_I)) activeButtons |= (1u << 12); // TRIANGLE
        if (IsKeyDown(KEY_UP)) activeButtons |= (1u << 4); // UP
        if (IsKeyDown(KEY_RIGHT)) activeButtons |= (1u << 5); // RIGHT
        if (IsKeyDown(KEY_DOWN)) activeButtons |= (1u << 6); // DOWN
        if (IsKeyDown(KEY_LEFT)) activeButtons |= (1u << 7); // LEFT
        if (IsKeyDown(KEY_Q)) activeButtons |= (1u << 10); // L1
        if (IsKeyDown(KEY_W)) activeButtons |= (1u << 8); // L2
        if (IsKeyDown(KEY_E)) activeButtons |= (1u << 11); // R1
        if (IsKeyDown(KEY_R)) activeButtons |= (1u << 9); // R2
        g_hostPadButtons.store(activeButtons, std::memory_order_release);

        extern std::atomic<uint64_t> g_gsFrameKicks;
        static uint64_t s_lastPresentedGsKicks = std::numeric_limits<uint64_t>::max();
        static auto s_lastPresentTime = std::chrono::steady_clock::now();
        static uint32_t s_fpsCounter = 0;
        static auto s_lastFpsLogTime = std::chrono::steady_clock::now();

        const uint64_t currentGsKicks = g_gsFrameKicks.load(std::memory_order_acquire);
        const auto now = std::chrono::steady_clock::now();
        const auto timeSinceLastPresent = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPresentTime).count();
        const bool shouldPresent = (currentGsKicks != s_lastPresentedGsKicks) || (timeSinceLastPresent >= 33);

        if (shouldPresent)
        {
            s_lastPresentedGsKicks = currentGsKicks;
            s_lastPresentTime = now;
            s_fpsCounter++;
            if (g_trackedCurVtable.load(std::memory_order_relaxed) == 0x006adfa8u)
            {
                g_movieSceneFrames.fetch_add(1, std::memory_order_relaxed);
            }

            const auto presT0 = std::chrono::steady_clock::now();
            uint32_t presentWidth = FB_WIDTH;
            uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
            UploadFrame(frameTex, this, presentWidth, presentHeight, tick);

            BeginDrawing();
            ClearBackground(BLACK);
            const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
            const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
            const float screenWidth = static_cast<float>(GetScreenWidth());
            const float screenHeight = static_cast<float>(GetScreenHeight());
            const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
            const float dstWidth = srcWidth * scale;
            const float dstHeight = srcHeight * scale;
            const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
            const Rectangle dstRect{
                (screenWidth - dstWidth) * 0.5f,
                (screenHeight - dstHeight) * 0.5f,
                dstWidth,
                dstHeight};
            DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            if (m_debugUiInitialized && m_debugUiDrawCallback)
            {
                m_debugUiDrawCallback(*this, m_debugUiUserData);
            }
            EndDrawing();
            const auto presT1 = std::chrono::steady_clock::now();
            s_profPresentUs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(presT1 - presT0).count());
        }
        else
        {
            PollInputEvents();
            std::this_thread::yield();
        }

        const auto timeSinceLastFpsLog = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastFpsLogTime).count();
        if (timeSinceLastFpsLog >= 1000)
        {
            uint32_t currentFps = static_cast<uint32_t>(s_fpsCounter * 1000.0 / timeSinceLastFpsLog);
            ps2xSafeLog("[fps] FPS: %u (GS frames/sec), kicks=%llu, vsyncTick=%llu\n",
                        currentFps, (unsigned long long)currentGsKicks, (unsigned long long)(m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0));

            uint64_t eeUs = g_profEeUs.exchange(0, std::memory_order_relaxed);
            uint64_t gsUs = g_profGsUs.exchange(0, std::memory_order_relaxed);
            uint64_t xferUs = g_profXferUs.exchange(0, std::memory_order_relaxed);
            uint64_t presentUs = s_profPresentUs;
            s_profPresentUs = 0;
            uint64_t prims = g_profPrims.exchange(0, std::memory_order_relaxed);
            uint64_t pixels = g_profPixels.exchange(0, std::memory_order_relaxed);

            double ee_ms = eeUs / 1000.0;
            double gs_ms = gsUs / 1000.0;
            double xfer_ms = xferUs / 1000.0;
            double present_ms = presentUs / 1000.0;

            ps2xSafeLog("[prof] fps=%u ee_ms=%.2f gs_ms=%.2f xfer_ms=%.2f present_ms=%.2f prims=%llu pixels=%llu\n",
                        currentFps, ee_ms, gs_ms, xfer_ms, present_ms,
                        (unsigned long long)prims, (unsigned long long)pixels);

            if (g_trackedCurVtable.load(std::memory_order_relaxed) == 0x006adfa8u)
            {
                const uint8_t *rdram = m_memory.getRDRAM();
                uint32_t s3 = g_trackedCurScene.load(std::memory_order_relaxed);
                uint32_t s3_state = 0;
                float s3_timer = 0.0f;
                uint32_t flag771bb8 = 0;
                char desc6e3890[17] = {0};
                if (rdram)
                {
                    if (s3 != 0 && s3 < PS2_RAM_SIZE - 0x200)
                    {
                        std::memcpy(&s3_state, rdram + ((s3 + 0xF4) & PS2_RAM_MASK), sizeof(s3_state));
                        std::memcpy(&s3_timer, rdram + ((s3 + 0x118) & PS2_RAM_MASK), sizeof(s3_timer));
                    }
                    std::memcpy(&flag771bb8, rdram + (0x00771bb8u & PS2_RAM_MASK), sizeof(flag771bb8));
                    std::memcpy(desc6e3890, rdram + (0x006e3890u & PS2_RAM_MASK), 16);
                }
                ps2xSafeLog("[movie] seekDone=%d reads=%u getPicture=%u isEnd=%u fallback=%d s3=0x%08x state=%u timer=%.2f flag771bb8=0x%x desc6e3890='%s'\n",
                            g_movieSeekDone.load() ? 1 : 0,
                            g_movieReadsCount.load(),
                            g_movieGetPictureCount.load(),
                            g_movieIsEndCount.load(),
                            g_movieFallbackTriggered.load() ? 1 : 0,
                            s3, s3_state, s3_timer, flag771bb8, desc6e3890);
            }

            const auto timeInScene = std::chrono::duration_cast<std::chrono::seconds>(now - g_lastSceneTransitionTime).count();
            static long long s_lastStallReported10s = 0;
            if (timeInScene >= 10 && (timeInScene / 10 != s_lastStallReported10s))
            {
                s_lastStallReported10s = timeInScene / 10;
                ps2xSafeLog("[stall:diag] Same scene for %llds: cur=0x%08x (vtable=0x%08x) FPS=%u. Last 5 distinct fns: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x.\n",
                            (long long)timeInScene, g_trackedCurScene.load(), g_trackedCurVtable.load(), currentFps,
                            g_lastDistinctFns[0], g_lastDistinctFns[1], g_lastDistinctFns[2], g_lastDistinctFns[3], g_lastDistinctFns[4]);
            }
            char titleBuf[128];
            std::snprintf(titleBuf, sizeof(titleBuf), "Ape Escape 3 Recomp - FPS: %u (GS %llu)", currentFps, (unsigned long long)currentGsKicks);
            SetWindowTitle(titleBuf);
            s_fpsCounter = 0;
            s_lastFpsLogTime = now;
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
