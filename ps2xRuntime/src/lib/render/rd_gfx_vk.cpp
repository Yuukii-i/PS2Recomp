// RDG2: Vulkan implementation of rdgfx:: interface.

#include "rd_gfx_vk.h"
#include "rd_enhance.h" // FEAT-ENH1

#if defined(PS2X_HAS_SDL3)

#ifdef _WIN32 // PROBE-RDG4P
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

VkInstance ps2xSdlVkInstance();
VkPhysicalDevice ps2xSdlVkPhysicalDevice();
VkDevice ps2xSdlVkDevice();
VkQueue ps2xSdlVkQueue();
uint32_t ps2xSdlVkQueueFamily();
VkFormat ps2xSdlVkSwapFormat();
uint32_t ps2xSdlVkFramesInFlight();
bool ps2xSdlVkBeginRdFrame(VkCommandBuffer *cb, VkImage *swapImage, uint32_t *w, uint32_t *h, uint32_t *frameIdx);
void ps2xSdlVkEndRdFrame();
double ps2xSdlVkLastBeginWaitMs(); // PROBE-RDG4P
VkImageView ps2xSdlVkCurrentSwapView();
bool ps2xSdlImGuiRecord(VkCommandBuffer cb, VkImage swapImage, VkImageView swapView, uint32_t w, uint32_t h);
extern int ps2xGfxGetAspectMode(); // FIX-P2
extern void ps2xSafeLog(const char *fmt, ...); // FEAT-ENH1: raw stderr is redirected to NUL
extern "C" bool ps2xRdDrawProfIn3d(); // PROBE-FG5
extern "C" void ps2xRdDrawProfAddVk(int64_t stateBindTicks, int64_t uniformTicks, int64_t drawCallTicks,
                                    uint64_t nStateChanges, uint64_t nUniformUpdates, uint64_t nDraws); // PROBE-FG5
extern "C" void ps2xRdDrawProfAddFg6(uint64_t skippedState, uint64_t skippedUniform); // FEAT-FG6
extern "C" void ps2xRdDrawProfAddFg7Pc(uint64_t pcUpdates, uint64_t pcBytes); // PROBE-FG7
extern "C" void ps2xRdDrawProfAddFg8PcWord(uint32_t wordIdx, uint32_t oldVal, uint32_t newVal); // PROBE-FG8
extern "C" void ps2xRdDrawProfAddFg9Bind(int64_t pipeLookTicks, uint64_t pipeLookCnt, uint64_t pipeMissCnt,
                                         int64_t bindPipeTicks, uint64_t bindPipeCnt,
                                         int64_t bindDescTicks, uint64_t bindDescCnt,
                                         int64_t bindVbIbTicks, uint64_t bindVbIbCnt,
                                         int64_t dynStateTicks, uint64_t dynStateCnt,
                                         int64_t pushConstTicks, uint64_t pushConstCnt); // PROBE-FG9
static inline bool fg6StateCacheEnabled() // FEAT-FG6
{
    static const bool s_on = []() {
        const char *e = std::getenv("PS2X_RD_STATECACHE");
        return e && e[0] == '1';
    }();
    return s_on;
}
static inline bool fg5ProfActive() // FEAT-FG6
{
    static const bool s_fg5On = []() {
        const char *e = std::getenv("PS2X_RD_DRAWPROF");
        return e && e[0] == '1';
    }();
    return s_fg5On && ps2xRdDrawProfIn3d();
}

// PROBE-V3B: Pause composite timing probe
namespace probe_v3b
{
    uint32_t n = 0;
    double aSum = 0.0, aMax = 0.0;
    double bSum = 0.0, bMax = 0.0;
    double cSum = 0.0, cMax = 0.0;
    double dSum = 0.0, dMax = 0.0;
    double eSum = 0.0, eMax = 0.0;
    double fSum = 0.0, fMax = 0.0;
    uint32_t waits = 0;
    double waitMs = 0.0;
    uint32_t allocs = 0;

    uint32_t cntQueueWait = 0, cntDevWait = 0, cntFenceWait = 0, cntMapMem = 0;
    double msQueueWait = 0.0, msDevWait = 0.0, msFenceWait = 0.0, msMapMem = 0.0;
    uint32_t cntAllocMem = 0, cntCrTex = 0;
    double msAllocMem = 0.0, msCrTex = 0.0;
    uint32_t cntAsyncUpload = 0; // FIX-V3D
} // PROBE-V3B

namespace rdgfx_vk
{
namespace
{

struct PushConstants
{
    float uFlip[4];
    float uVpA[4];
    float uVpB[4];
    float uFbSize[2];
    float uVpZ[2];
    float uZBias;
    float uFixAlpha;
    int32_t uMode2d;
    int32_t uAtst;
    int32_t uAref;
    int32_t uAtInvert;
    int32_t uTex;
    int32_t uTfx;
    int32_t uTcc;
    int32_t uPalMode;
    int32_t uPalFallback;
    int32_t _pad0;
    float uFog[4]; // FIX-E1c
};
static_assert(sizeof(PushConstants) <= 128, "PushConstants must fit in 128 bytes");
static_assert(sizeof(PushConstants) == 128, "PushConstants must be 128 bytes"); // FIX-E1c

struct PipelineKey
{
    bool blendEnable = false;
    rdgfx::BlendFactor srcRgb = rdgfx::BlendFactor::One;
    rdgfx::BlendFactor dstRgb = rdgfx::BlendFactor::Zero;
    rdgfx::BlendFactor srcAlpha = rdgfx::BlendFactor::One;
    rdgfx::BlendFactor dstAlpha = rdgfx::BlendFactor::Zero;
    rdgfx::BlendEq eqRgb = rdgfx::BlendEq::Add;
    rdgfx::BlendEq eqAlpha = rdgfx::BlendEq::Add;
    uint8_t colorMask = 0x0Fu;
    bool depthTest = false;
    bool depthMask = true;
    rdgfx::DepthFunc depthFunc = rdgfx::DepthFunc::Lequal;
    bool cull = false;
    uint8_t samples = 1; // RDG2B

    uint64_t pack() const
    {
        uint64_t k = 0;
        k |= (blendEnable ? 1ull : 0ull);
        k |= (static_cast<uint64_t>(srcRgb) & 7ull) << 1;
        k |= (static_cast<uint64_t>(dstRgb) & 7ull) << 4;
        k |= (static_cast<uint64_t>(srcAlpha) & 7ull) << 7;
        k |= (static_cast<uint64_t>(dstAlpha) & 7ull) << 10;
        k |= (static_cast<uint64_t>(eqRgb) & 3ull) << 13;
        k |= (static_cast<uint64_t>(eqAlpha) & 3ull) << 15;
        k |= (static_cast<uint64_t>(colorMask) & 0xFull) << 17;
        k |= (depthTest ? 1ull : 0ull) << 21;
        k |= (depthMask ? 1ull : 0ull) << 22;
        k |= (static_cast<uint64_t>(depthFunc) & 1ull) << 23;
        k |= (cull ? 1ull : 0ull) << 24;
        k |= (static_cast<uint64_t>(samples) & 0x7Full) << 25; // RDG2B
        return k;
    }
};

struct SamplerKey
{
    rdgfx::TextureFilter minFilter = rdgfx::TextureFilter::Nearest;
    rdgfx::TextureFilter magFilter = rdgfx::TextureFilter::Nearest;
    rdgfx::TextureWrap wrapS = rdgfx::TextureWrap::Repeat;
    rdgfx::TextureWrap wrapT = rdgfx::TextureWrap::Repeat;

    uint32_t pack() const
    {
        uint32_t k = 0;
        k |= (static_cast<uint32_t>(minFilter) & 3u);
        k |= (static_cast<uint32_t>(magFilter) & 3u) << 2;
        k |= (static_cast<uint32_t>(wrapS) & 1u) << 4;
        k |= (static_cast<uint32_t>(wrapT) & 1u) << 5;
        return k;
    }
};

struct BufferCopy
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
};

struct PendingBufferWrite
{
    int offset = 0;
    std::vector<uint8_t> data;
};

struct GpuBuffer
{
    int sizeBytes = 0;
    bool isIndex = false;
    std::vector<BufferCopy> copies;
    std::vector<std::vector<PendingBufferWrite>> pending;
};

struct GpuTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
    uint32_t mipLevels = 1;
    SamplerKey sampler;
    uint64_t memBytes = 0; // FIX-V3E
};

struct GarbageBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// FIX-V3E: pool recycled texture images instead of create/alloc/destroy
struct PooledImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t mips = 0;
    uint64_t bytes = 0;
    uint64_t lastUsedSerial = 0;
};

static inline uint64_t imagePoolKey(uint32_t w, uint32_t h, uint32_t mips)
{
    return (static_cast<uint64_t>(w) << 32) | (static_cast<uint64_t>(h) << 16) | (static_cast<uint64_t>(mips) & 0xFFFF);
}

struct GarbageTexture
{
    VkImageView view = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t w = 0; // FIX-V3E
    uint32_t h = 0; // FIX-V3E
    uint32_t mips = 0; // FIX-V3E
    uint64_t bytes = 0; // FIX-V3E
};

// FIX-RDG3C / FIX-FG9
struct DescCacheKey
{
    uint32_t tex0 = 0;
    uint32_t tex1 = 0;
    uint32_t samp0 = 0;
    uint32_t samp1 = 0;
    uint64_t hash64 = 0; // FIX-FG9

    void computeHash() // FIX-FG9
    {
        uint64_t h1 = (static_cast<uint64_t>(tex0) << 32) | tex1;
        uint64_t h2 = (static_cast<uint64_t>(samp0) << 32) | samp1;
        h1 ^= h2 * 0x9e3779b97f4a7c15ull;
        h1 ^= h1 >> 30;
        h1 *= 0xbf58476d1ce4e5b9ull;
        h1 ^= h1 >> 27;
        hash64 = h1;
    }

    bool operator==(const DescCacheKey &o) const
    {
        return hash64 == o.hash64 && tex0 == o.tex0 && tex1 == o.tex1 && samp0 == o.samp0 && samp1 == o.samp1;
    }
};

struct DescCacheKeyHash
{
    size_t operator()(const DescCacheKey &k) const
    {
        return static_cast<size_t>(k.hash64); // FIX-FG9
    }
};

struct GpuVao
{
    uint32_t vbo = 0;
    uint32_t ebo = 0;
    VkDeviceSize vbOffset = 0;
};

// PERF-RDG4R: flat vector handle table with free-list (0 is always invalid).
template <typename T>
struct HandleTable
{
    struct Slot
    {
        bool alive = false;
        T value{};
    };
    std::vector<Slot> slots;
    std::vector<uint32_t> freeList;

    uint32_t allocate(T &&val)
    {
        uint32_t id = 0;
        if (!freeList.empty())
        {
            id = freeList.back();
            freeList.pop_back();
            slots[id].alive = true;
            slots[id].value = std::move(val);
        }
        else
        {
            if (slots.empty())
                slots.emplace_back(); // slot 0 reserved as invalid
            id = static_cast<uint32_t>(slots.size());
            slots.push_back(Slot{true, std::move(val)});
        }
        return id;
    }

    T *find(uint32_t id)
    {
        if (id == 0 || id >= slots.size() || !slots[id].alive)
            return nullptr;
        return &slots[id].value;
    }

    const T *find(uint32_t id) const
    {
        if (id == 0 || id >= slots.size() || !slots[id].alive)
            return nullptr;
        return &slots[id].value;
    }

    void erase(uint32_t id)
    {
        if (id == 0 || id >= slots.size() || !slots[id].alive)
            return;
        slots[id].alive = false;
        slots[id].value = T{};
        freeList.push_back(id);
    }
};

struct RdVkState
{
    bool initialized = false;
    bool shaderReady = false;

    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t framesInFlight = 2;

    VkCommandPool uploadPool = VK_NULL_HANDLE;

    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> pipelines;

    // FIX-RDG3C
    std::vector<std::vector<VkDescriptorPool>> descPools;
    std::vector<size_t> descPoolIdx;
    std::vector<std::unordered_map<DescCacheKey, VkDescriptorSet, DescCacheKeyHash>> descCaches;
    bool descAllocFailLogged = false;
    std::unordered_map<uint32_t, VkSampler> samplers;

    // Offscreen render targets
    uint32_t targetW = 0;
    uint32_t targetH = 0;
    uint32_t swapW = 0; // FIX-R2
    uint32_t swapH = 0; // FIX-R2
    VkImage colorImage = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // RDG2B: MSAA resolve target
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkImage resolveImage = VK_NULL_HANDLE;
    VkDeviceMemory resolveMem = VK_NULL_HANDLE;
    VkImageView resolveView = VK_NULL_HANDLE;
    VkImageLayout resolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // RDG2B: palette textures & overlay VBO
    std::vector<uint32_t> palTexIds;
    std::vector<std::vector<uint32_t>> palLastRgba;
    std::vector<bool> palHasData;
    uint32_t overlayVbo = 0;
    uint32_t overlayVertexCursor = 0;
    uint32_t classicTexId = 0; // FXC1
    int classicTexW = 0; // FXC1
    int classicTexH = 0; // FXC1

    // Frame state
    bool frameActive = false;
    bool inRenderPass = false;
    bool firstPassInFrame = true;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage swapImage = VK_NULL_HANDLE;
    uint32_t curFrame = 0;

    // Dynamic render state
    PushConstants pc{};
    PipelineKey pipeKey{};
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    int vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    bool scissorTest = false;
    int scX = 0, scY = 0, scW = 0, scH = 0;
    uint32_t boundVbo = 0;
    uint32_t boundEbo = 0;
    uint32_t boundTex[2] = {0, 0};

    // PERF-RDG4R: dirty flags, cached resolved resources, and last-bound command-buffer state
    bool dirtyPipeline = true;
    bool dirtyViewport = true;
    bool dirtyScissor = true;
    bool dirtyTextures = true;
    bool dirtyPushConstants = true;
    bool dirtyVertexBuffer = true;
    bool dirtyIndexBuffer = true;

    VkPipeline cachedPipeline = VK_NULL_HANDLE;
    const GpuTexture *cachedTex[2] = {nullptr, nullptr};
    VkSampler cachedSampler[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t cachedEffTexId[2] = {0, 0};
    VkDescriptorSet cachedDescSet = VK_NULL_HANDLE;
    uint64_t lastPipePacked = 0; // FIX-FG9
    VkPipeline lastPipeHandle = VK_NULL_HANDLE; // FIX-FG9
    bool lastPipeValid = false; // FIX-FG9
    DescCacheKey lastDescKey{}; // FIX-FG9
    VkDescriptorSet lastDescCacheSet = VK_NULL_HANDLE; // FIX-FG9
    uint32_t lastDescCacheFrame = UINT32_MAX; // FIX-FG9

    VkDescriptorSet lastBoundDescSet = VK_NULL_HANDLE;
    bool lastPcValid = false;
    PushConstants lastPc{};
    int32_t curAtInvert = 0; // FIX-FG8
    bool lastVpValid = false;
    VkViewport lastVp{};
    bool lastScValid = false;
    VkRect2D lastSc{};
    VkBuffer lastBoundVb = VK_NULL_HANDLE;
    VkDeviceSize lastBoundVbOffset = 0;
    VkBuffer lastBoundIb = VK_NULL_HANDLE;
    VkIndexType lastBoundIbType = VK_INDEX_TYPE_MAX_ENUM;

    // FIX-RDG3C / PERF-RDG4R
    uint32_t boundVao = 0;
    HandleTable<GpuVao> vaos;

    // Resource tables
    HandleTable<GpuBuffer> vbos;
    HandleTable<GpuBuffer> ebos;

    HandleTable<GpuTexture> textures;
    uint32_t defaultWhiteTex = 0;

    std::vector<std::vector<GarbageBuffer>> garbageBuffers;
    std::vector<std::vector<GarbageTexture>> garbageTextures;
    // FIX-V3M: garbage destruction backlog
    std::vector<GarbageBuffer> garbageBufBacklog;
    std::vector<GarbageTexture> garbageTexBacklog;
    size_t statBacklogMax = 0;
    std::vector<std::vector<VkCommandBuffer>> garbageCmds; // FIX-V3D
    std::vector<VkCommandBuffer> frameUploadCmd; // FIX-V3G
    std::vector<BufferCopy> stagingRings; // FIX-V3D
    std::vector<VkDeviceSize> stagingCursors; // FIX-V3D

    // Debug stats
    bool logEnabled = false;
    uint64_t statFrames = 0;
    uint64_t statDraws = 0;
    uint64_t totalBufBytes = 0;
    std::chrono::steady_clock::time_point lastLogTime{};

    // PROBE-RDG3B
    uint64_t statDrawIdx = 0;
    uint64_t statDrawArr = 0;
    uint64_t statNoFrame = 0;
    uint64_t statNoPass = 0;
    uint64_t statNoPipeline = 0;
    uint64_t statNoVb = 0;
    uint64_t statNoIb = 0;
    uint64_t statNoDesc = 0;
    uint64_t statZeroCount = 0;
    uint64_t statClears = 0;
    uint64_t statBinds = 0;
    uint64_t statTexCreated = 0;
    uint64_t statVbUpd = 0;
    uint64_t statUniformSets = 0;
    uint64_t statPassBegun = 0;
    bool firstDrawLogged = false;

    // FEAT-ENH2: transform of this frame's largest 3D draw (tracked only with a debug view)
    bool enhTrack = false;
    uint32_t enh3dCount = 0;
    float enhFlip[4] = {};
    float enhVpA[4] = {};
    float enhVpB[4] = {};
    float enhFbSize[2] = {};
    float enhZBias = 0.0f;
    VkViewport enh3dViewport{};

    // PROBE-RDG4P
    int probeDrawDepth = 0;
    int probeTexDepth = 0;
    int64_t recStartQpc = 0;
    double curFrameWaitMs = 0.0;
    double curFrameRecMs = 0.0;
    int64_t curFrameDrawTicks = 0;
    int64_t curFrameTexTicks = 0;
    double curFrameEndMs = 0.0;
    uint64_t curFrameDraws = 0;
    uint64_t curFrameDescHits = 0;
    uint64_t curFrameDescMisses = 0;

    uint64_t winFrames = 0;
    double winWaitSumMs = 0.0, winWaitMaxMs = 0.0;
    double winRecSumMs = 0.0, winRecMaxMs = 0.0;
    double winDrawSumMs = 0.0, winDrawMaxMs = 0.0;
    double winTexSumMs = 0.0, winTexMaxMs = 0.0;
    double winEndSumMs = 0.0, winEndMaxMs = 0.0;
    uint64_t winDrawsSum = 0, winDrawsMax = 0;
    uint64_t winDescHitsSum = 0, winDescHitsMax = 0;
    uint64_t winDescMissesSum = 0, winDescMissesMax = 0;

    // FIX-V3E: texture image recycling pool
    std::unordered_multimap<uint64_t, PooledImage> imagePool;
    uint64_t poolTotalBytes = 0;
    uint64_t poolSerial = 0;
    uint64_t statTexPoolHit = 0;
    uint64_t statTexPoolMiss = 0;

    void destroyImagePool() // FIX-V3E
    {
        if (device == VK_NULL_HANDLE)
            return;
        for (auto &pair : imagePool)
        {
            PooledImage &pi = pair.second;
            if (pi.view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device, pi.view, nullptr);
                pi.view = VK_NULL_HANDLE;
            }
            if (pi.image != VK_NULL_HANDLE)
            {
                vkDestroyImage(device, pi.image, nullptr);
                pi.image = VK_NULL_HANDLE;
            }
            if (pi.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(device, pi.memory, nullptr);
                pi.memory = VK_NULL_HANDLE;
            }
        }
        imagePool.clear();
        poolTotalBytes = 0;
    }

    void drainGarbageBacklog() // FIX-V3M
    {
        if (device == VK_NULL_HANDLE)
            return;
        for (const auto &gb : garbageBufBacklog)
        {
            if (gb.buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, gb.buffer, nullptr);
            if (gb.memory != VK_NULL_HANDLE)
                vkFreeMemory(device, gb.memory, nullptr);
        }
        garbageBufBacklog.clear();

        for (const auto &gt : garbageTexBacklog)
        {
            if (gt.view != VK_NULL_HANDLE)
                vkDestroyImageView(device, gt.view, nullptr);
            if (gt.image != VK_NULL_HANDLE)
                vkDestroyImage(device, gt.image, nullptr);
            if (gt.memory != VK_NULL_HANDLE)
                vkFreeMemory(device, gt.memory, nullptr);
        }
        garbageTexBacklog.clear();
    }

    ~RdVkState() // FIX-V3E
    {
        drainGarbageBacklog(); // FIX-V3M
        destroyImagePool();
    }
};

RdVkState s_vk;

// PROBE-RDG4P
inline int64_t probeQpcNow()
{
#ifdef _WIN32
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return static_cast<int64_t>(li.QuadPart);
#else
    return 0;
#endif
}

inline double probeQpcToMs(int64_t ticks)
{
#ifdef _WIN32
    static const double invFreqMs = []()
    {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart > 0 ? (1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    }();
    return static_cast<double>(ticks) * invFreqMs;
#else
    (void)ticks;
    return 0.0;
#endif
}

struct ProbeDrawScope
{
    bool top = false;
    int64_t t0 = 0;
    ProbeDrawScope()
    {
        if (s_vk.logEnabled && s_vk.probeDrawDepth++ == 0)
        {
            top = true;
            t0 = probeQpcNow();
        }
    }
    ~ProbeDrawScope()
    {
        if (s_vk.logEnabled)
        {
            if (top)
                s_vk.curFrameDrawTicks += (probeQpcNow() - t0);
            --s_vk.probeDrawDepth;
        }
    }
};

struct ProbeTexScope
{
    bool top = false;
    int64_t t0 = 0;
    ProbeTexScope()
    {
        if (s_vk.logEnabled && s_vk.probeTexDepth++ == 0)
        {
            top = true;
            t0 = probeQpcNow();
        }
    }
    ~ProbeTexScope()
    {
        if (s_vk.logEnabled)
        {
            if (top)
                s_vk.curFrameTexTicks += (probeQpcNow() - t0);
            --s_vk.probeTexDepth;
        }
    }
};

uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(s_vk.physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

void imageBarrier(VkCommandBuffer cmd,
                  VkImage image,
                  VkImageAspectFlags aspect,
                  uint32_t baseMip,
                  uint32_t mipCount,
                  VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout,
                  VkImageLayout newLayout)
{
    ProbeTexScope texProbe; // PROBE-RDG4P
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange = {aspect, baseMip, mipCount, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkCommandBuffer beginOneTimeCmd()
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s_vk.uploadPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s_vk.device, &ai, &cmd) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void endOneTimeCmd(VkCommandBuffer cmd)
{
    if (cmd == VK_NULL_HANDLE)
        return;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(s_vk.queue, 1, &si, VK_NULL_HANDLE);
    const auto t_w0 = std::chrono::steady_clock::now(); // PROBE-V3B
    vkQueueWaitIdle(s_vk.queue);
    const auto t_w1 = std::chrono::steady_clock::now(); // PROBE-V3B
    const double ms = std::chrono::duration<double, std::milli>(t_w1 - t_w0).count(); // PROBE-V3B
    probe_v3b::cntQueueWait++; // PROBE-V3B
    probe_v3b::msQueueWait += ms; // PROBE-V3B
    probe_v3b::waits++; // PROBE-V3B
    probe_v3b::waitMs += ms; // PROBE-V3B
    vkFreeCommandBuffers(s_vk.device, s_vk.uploadPool, 1, &cmd);
}

void endOneTimeCmdAsync(VkCommandBuffer cmd) // FIX-V3D
{
    if (cmd == VK_NULL_HANDLE)
        return;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(s_vk.queue, 1, &si, VK_NULL_HANDLE);
    probe_v3b::cntAsyncUpload++; // FIX-V3D
    s_vk.garbageCmds[s_vk.curFrame].push_back(cmd);
}

VkCommandBuffer beginUploadCmdBatched() // FIX-V3G
{
    if (!s_vk.frameActive)
        return beginOneTimeCmd();

    const uint32_t f = s_vk.curFrame;
    if (f < s_vk.frameUploadCmd.size() && s_vk.frameUploadCmd[f] != VK_NULL_HANDLE)
        return s_vk.frameUploadCmd[f];

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s_vk.uploadPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s_vk.device, &ai, &cmd) != VK_SUCCESS)
        return beginOneTimeCmd();

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(s_vk.device, s_vk.uploadPool, 1, &cmd);
        return beginOneTimeCmd();
    }

    if (f < s_vk.frameUploadCmd.size())
        s_vk.frameUploadCmd[f] = cmd;
    return cmd;
}

void endUploadCmdBatched(VkCommandBuffer cmd) // FIX-V3G
{
    if (cmd == VK_NULL_HANDLE)
        return;
    if (s_vk.frameActive && s_vk.curFrame < s_vk.frameUploadCmd.size() && cmd == s_vk.frameUploadCmd[s_vk.curFrame])
        return; // Keep open for batching across the frame
    endOneTimeCmd(cmd);
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path)
{
    FILE *fp = std::fopen(path.string().c_str(), "rb");
    if (!fp)
        return {};
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz <= 0)
    {
        std::fclose(fp);
        return {};
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t n = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (n != buf.size())
        return {};
    return buf;
}

VkShaderModule createShaderModuleFromFile(const std::filesystem::path &path)
{
    auto bytes = readBinaryFile(path);
    if (bytes.empty() || (bytes.size() % 4) != 0)
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(s_vk.device, &ci, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

std::filesystem::path getExeDir()
{
    const char *base = SDL_GetBasePath();
    if (base && base[0] != '\0')
        return std::filesystem::path(base);
    return std::filesystem::current_path();
}

void enhanceDestroyTargets(); // FEAT-ENH1 (rd_enhance_vk.inc)
void enhanceVerifyCollect(uint32_t frameIdx); // PROBE-ENH1

void destroyTargets()
{
    if (s_vk.device == VK_NULL_HANDLE)
        return;
    enhanceDestroyTargets(); // FEAT-ENH1
    if (s_vk.colorView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(s_vk.device, s_vk.colorView, nullptr);
        s_vk.colorView = VK_NULL_HANDLE;
    }
    if (s_vk.colorImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(s_vk.device, s_vk.colorImage, nullptr);
        s_vk.colorImage = VK_NULL_HANDLE;
    }
    if (s_vk.colorMem != VK_NULL_HANDLE)
    {
        vkFreeMemory(s_vk.device, s_vk.colorMem, nullptr);
        s_vk.colorMem = VK_NULL_HANDLE;
    }
    if (s_vk.depthView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(s_vk.device, s_vk.depthView, nullptr);
        s_vk.depthView = VK_NULL_HANDLE;
    }
    if (s_vk.depthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(s_vk.device, s_vk.depthImage, nullptr);
        s_vk.depthImage = VK_NULL_HANDLE;
    }
    if (s_vk.depthMem != VK_NULL_HANDLE)
    {
        vkFreeMemory(s_vk.device, s_vk.depthMem, nullptr);
        s_vk.depthMem = VK_NULL_HANDLE;
    }
    if (s_vk.resolveView != VK_NULL_HANDLE) // RDG2B
    {
        vkDestroyImageView(s_vk.device, s_vk.resolveView, nullptr);
        s_vk.resolveView = VK_NULL_HANDLE;
    }
    if (s_vk.resolveImage != VK_NULL_HANDLE) // RDG2B
    {
        vkDestroyImage(s_vk.device, s_vk.resolveImage, nullptr);
        s_vk.resolveImage = VK_NULL_HANDLE;
    }
    if (s_vk.resolveMem != VK_NULL_HANDLE) // RDG2B
    {
        vkFreeMemory(s_vk.device, s_vk.resolveMem, nullptr);
        s_vk.resolveMem = VK_NULL_HANDLE;
    }
    s_vk.colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.resolveLayout = VK_IMAGE_LAYOUT_UNDEFINED; // RDG2B
    s_vk.targetW = 0;
    s_vk.targetH = 0;
    s_vk.swapW = 0; // FIX-R2
    s_vk.swapH = 0; // FIX-R2
}

bool createImageAndView(uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, VkImage &outImg, VkDeviceMemory &outMem, VkImageView &outView)
{
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = samples; // RDG2B
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_vk.device, &ici, nullptr, &outImg) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(s_vk.device, outImg, &req);
    uint32_t memType = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == UINT32_MAX)
        return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(s_vk.device, &mai, nullptr, &outMem) != VK_SUCCESS)
        return false;
    if (vkBindImageMemory(s_vk.device, outImg, outMem, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = outImg;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = fmt;
    ivci.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(s_vk.device, &ivci, nullptr, &outView) == VK_SUCCESS;
}

bool ensureTargets(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return false;
    if (s_vk.colorImage != VK_NULL_HANDLE && s_vk.targetW == w && s_vk.targetH == h)
        return true;

    const auto t_dw0 = std::chrono::steady_clock::now(); // PROBE-V3B
    vkDeviceWaitIdle(s_vk.device);
    const auto t_dw1 = std::chrono::steady_clock::now(); // PROBE-V3B
    const double dwMs = std::chrono::duration<double, std::milli>(t_dw1 - t_dw0).count(); // PROBE-V3B
    probe_v3b::cntDevWait++; // PROBE-V3B
    probe_v3b::msDevWait += dwMs; // PROBE-V3B
    probe_v3b::waits++; // PROBE-V3B
    probe_v3b::waitMs += dwMs; // PROBE-V3B
    s_vk.drainGarbageBacklog(); // FIX-V3M
    destroyTargets();

    const bool useMsaa = (s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT); // RDG2B
    const VkImageUsageFlags colorUsage = useMsaa
                                             ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                             : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    if (!createImageAndView(w, h, VK_FORMAT_R8G8B8A8_UNORM, s_vk.msaaSamples, colorUsage,
                            VK_IMAGE_ASPECT_COLOR_BIT, s_vk.colorImage, s_vk.colorMem, s_vk.colorView))
    {
        destroyTargets();
        return false;
    }

    if (!createImageAndView(w, h, VK_FORMAT_D32_SFLOAT, s_vk.msaaSamples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, // FEAT-ENH1
                            VK_IMAGE_ASPECT_DEPTH_BIT, s_vk.depthImage, s_vk.depthMem, s_vk.depthView))
    {
        destroyTargets();
        return false;
    }

    if (useMsaa) // RDG2B
    {
        if (!createImageAndView(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, s_vk.resolveImage, s_vk.resolveMem, s_vk.resolveView))
        {
            destroyTargets();
            return false;
        }
    }

    s_vk.targetW = w;
    s_vk.targetH = h;
    s_vk.colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.resolveLayout = VK_IMAGE_LAYOUT_UNDEFINED; // RDG2B
    s_vk.vpX = 0;
    s_vk.vpY = 0;
    s_vk.vpW = static_cast<int>(w);
    s_vk.vpH = static_cast<int>(h);
    return true;
}

VkRect2D computeVkScissor()
{
    const int tw = static_cast<int>(s_vk.targetW);
    const int th = static_cast<int>(s_vk.targetH);
    if (!s_vk.scissorTest)
        return {{0, 0}, {s_vk.targetW, s_vk.targetH}};

    int x = s_vk.scX;
    int y = th - (s_vk.scY + s_vk.scH);
    int w = s_vk.scW;
    int h = s_vk.scH;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > tw) w = tw - x;
    if (y + h > th) h = th - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return {{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
}

// PERF-RDG4R: invalidate all command-buffer tracked state and mark all dirty flags.
void invalidateCommandBufferState()
{
    s_vk.boundPipeline = VK_NULL_HANDLE;
    s_vk.lastBoundDescSet = VK_NULL_HANDLE;
    s_vk.lastPcValid = false;
    s_vk.lastVpValid = false;
    s_vk.lastScValid = false;
    s_vk.lastBoundVb = VK_NULL_HANDLE;
    s_vk.lastBoundVbOffset = 0;
    s_vk.lastBoundIb = VK_NULL_HANDLE;
    s_vk.lastBoundIbType = VK_INDEX_TYPE_MAX_ENUM;

    s_vk.dirtyPipeline = true;
    s_vk.dirtyViewport = true;
    s_vk.dirtyScissor = true;
    s_vk.dirtyTextures = true;
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
    s_vk.dirtyVertexBuffer = true;
    s_vk.dirtyIndexBuffer = true;
}

void applyViewportAndScissorIfDirty(uint64_t *outCalls = nullptr) // PROBE-FG9
{
    if (!s_vk.frameActive)
        return;
    if (s_vk.dirtyViewport || !s_vk.lastVpValid)
    {
        const int th = static_cast<int>(s_vk.targetH);
        VkViewport vp{};
        vp.x = static_cast<float>(s_vk.vpX);
        vp.y = static_cast<float>(th - (s_vk.vpY + s_vk.vpH));
        vp.width = static_cast<float>(s_vk.vpW);
        vp.height = static_cast<float>(s_vk.vpH);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        if (!s_vk.lastVpValid || std::memcmp(&vp, &s_vk.lastVp, sizeof(VkViewport)) != 0)
        {
            vkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
            s_vk.lastVp = vp;
            s_vk.lastVpValid = true;
            if (outCalls) ++(*outCalls); // PROBE-FG9
        }
        s_vk.dirtyViewport = false;
    }

    if (s_vk.dirtyScissor || !s_vk.lastScValid)
    {
        VkRect2D sc = computeVkScissor();
        if (!s_vk.lastScValid ||
            sc.offset.x != s_vk.lastSc.offset.x || sc.offset.y != s_vk.lastSc.offset.y ||
            sc.extent.width != s_vk.lastSc.extent.width || sc.extent.height != s_vk.lastSc.extent.height)
        {
            vkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
            s_vk.lastSc = sc;
            s_vk.lastScValid = true;
            if (outCalls) ++(*outCalls); // PROBE-FG9
        }
        s_vk.dirtyScissor = false;
    }
}

void ensureRenderPass(bool forceClear, float r, float g, float b, float a)
{
    if (!s_vk.frameActive || s_vk.inRenderPass)
        return;

    if (s_vk.colorLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        imageBarrier(s_vk.cmd, s_vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     s_vk.colorLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        s_vk.colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (s_vk.depthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        imageBarrier(s_vk.cmd, s_vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     s_vk.depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        s_vk.depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    const bool useMsaa = (s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT && s_vk.resolveView != VK_NULL_HANDLE); // RDG2B
    if (useMsaa && s_vk.resolveLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        imageBarrier(s_vk.cmd, s_vk.resolveImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     s_vk.resolveLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        s_vk.resolveLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    const bool doClear = s_vk.firstPassInFrame || forceClear;

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = s_vk.colorView;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = doClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color.float32[0] = r;
    colorAtt.clearValue.color.float32[1] = g;
    colorAtt.clearValue.color.float32[2] = b;
    colorAtt.clearValue.color.float32[3] = a;
    if (useMsaa) // RDG2B
    {
        colorAtt.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        colorAtt.resolveImageView = s_vk.resolveView;
        colorAtt.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = s_vk.depthView;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = doClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {s_vk.targetW, s_vk.targetH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;

    vkCmdBeginRendering(s_vk.cmd, &ri);
    ++s_vk.statPassBegun; // PROBE-RDG3B
    s_vk.inRenderPass = true;
    s_vk.firstPassInFrame = false;
    invalidateCommandBufferState(); // PERF-RDG4R
}

VkBlendFactor toVkBlendFactor(rdgfx::BlendFactor f)
{
    switch (f)
    {
    case rdgfx::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case rdgfx::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case rdgfx::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case rdgfx::BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case rdgfx::BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case rdgfx::BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp toVkBlendOp(rdgfx::BlendEq eq)
{
    switch (eq)
    {
    case rdgfx::BlendEq::Add: return VK_BLEND_OP_ADD;
    case rdgfx::BlendEq::Subtract: return VK_BLEND_OP_SUBTRACT;
    case rdgfx::BlendEq::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    }
    return VK_BLEND_OP_ADD;
}

VkPipeline getOrCreatePipeline(const PipelineKey &key, bool *outMiss = nullptr) // PROBE-FG9
{
    const uint64_t packed = key.pack();
    if (fg6StateCacheEnabled() && s_vk.lastPipeValid && s_vk.lastPipePacked == packed) // FIX-FG9
        return s_vk.lastPipeHandle;
    auto it = s_vk.pipelines.find(packed);
    if (it != s_vk.pipelines.end())
    {
        if (fg6StateCacheEnabled()) // FIX-FG9
        {
            s_vk.lastPipePacked = packed;
            s_vk.lastPipeHandle = it->second;
            s_vk.lastPipeValid = true;
        }
        return it->second;
    }
    if (outMiss) *outMiss = true; // PROBE-FG9

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s_vk.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s_vk.fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 32; // FIX-E1b
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{}; // FIX-E1b
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 16};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 20};
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, 28}; // FIX-E1b

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4; // FIX-E1b
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = key.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(key.samples ? key.samples : 1); // RDG2B

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = key.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key.depthMask ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (key.depthFunc == rdgfx::DepthFunc::Always) ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.blendEnable = key.blendEnable ? VK_TRUE : VK_FALSE;
    cbAtt.srcColorBlendFactor = toVkBlendFactor(key.srcRgb);
    cbAtt.dstColorBlendFactor = toVkBlendFactor(key.dstRgb);
    cbAtt.colorBlendOp = toVkBlendOp(key.eqRgb);
    cbAtt.srcAlphaBlendFactor = toVkBlendFactor(key.srcAlpha);
    cbAtt.dstAlphaBlendFactor = toVkBlendFactor(key.dstAlpha);
    cbAtt.alphaBlendOp = toVkBlendOp(key.eqAlpha);
    cbAtt.colorWriteMask = static_cast<VkColorComponentFlags>(key.colorMask & 0xFu);

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAtt;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rend{};
    rend.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &colorFmt;
    rend.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rend;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = s_vk.pipelineLayout;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    s_vk.pipelines[packed] = pipe;
    if (fg6StateCacheEnabled()) // FIX-FG9
    {
        s_vk.lastPipePacked = packed;
        s_vk.lastPipeHandle = pipe;
        s_vk.lastPipeValid = true;
    }
    return pipe;
}

VkSampler getOrCreateSampler(const SamplerKey &key)
{
    const uint32_t packed = key.pack();
    auto it = s_vk.samplers.find(packed);
    if (it != s_vk.samplers.end())
        return it->second;

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = (key.magFilter == rdgfx::TextureFilter::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = (key.minFilter == rdgfx::TextureFilter::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.mipmapMode = (key.minFilter == rdgfx::TextureFilter::MipLinear)
                         ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                         : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = (key.wrapS == rdgfx::TextureWrap::Clamp)
                           ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                           : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = (key.wrapT == rdgfx::TextureWrap::Clamp)
                           ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                           : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = (key.minFilter == rdgfx::TextureFilter::MipLinear) ? 16.0f : 0.0f;

    VkSampler samp = VK_NULL_HANDLE;
    if (vkCreateSampler(s_vk.device, &sci, nullptr, &samp) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    s_vk.samplers[packed] = samp;
    return samp;
}

bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, BufferCopy &out)
{
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_vk.device, &bci, nullptr, &out.buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(s_vk.device, out.buffer, &req);
    uint32_t memType = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == UINT32_MAX)
        return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType;
    const auto t_am0 = std::chrono::steady_clock::now(); // PROBE-V3B
    const VkResult allocRes = vkAllocateMemory(s_vk.device, &mai, nullptr, &out.memory); // PROBE-V3B
    const auto t_am1 = std::chrono::steady_clock::now(); // PROBE-V3B
    const double amMs = std::chrono::duration<double, std::milli>(t_am1 - t_am0).count(); // PROBE-V3B
    probe_v3b::cntAllocMem++; // PROBE-V3B
    probe_v3b::msAllocMem += amMs; // PROBE-V3B
    probe_v3b::allocs++; // PROBE-V3B
    if (allocRes != VK_SUCCESS)
        return false;
    if (vkBindBufferMemory(s_vk.device, out.buffer, out.memory, 0) != VK_SUCCESS)
        return false;
    const auto t_mm0 = std::chrono::steady_clock::now(); // PROBE-V3B
    const VkResult mapRes = vkMapMemory(s_vk.device, out.memory, 0, size, 0, &out.mapped); // PROBE-V3B
    const auto t_mm1 = std::chrono::steady_clock::now(); // PROBE-V3B
    const double mmMs = std::chrono::duration<double, std::milli>(t_mm1 - t_mm0).count(); // PROBE-V3B
    probe_v3b::cntMapMem++; // PROBE-V3B
    probe_v3b::msMapMem += mmMs; // PROBE-V3B
    probe_v3b::waits++; // PROBE-V3B
    probe_v3b::waitMs += mmMs; // PROBE-V3B
    if (mapRes != VK_SUCCESS)
        return false;
    return true;
}

uint32_t createDynBuffer(const void *data, int sizeBytes, bool isIndex)
{
    if (!s_vk.initialized || sizeBytes <= 0)
        return 0;

    GpuBuffer buf;
    buf.sizeBytes = sizeBytes;
    buf.isIndex = isIndex;
    buf.copies.resize(s_vk.framesInFlight);
    buf.pending.resize(s_vk.framesInFlight);

    const VkBufferUsageFlags usage = isIndex ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    for (uint32_t i = 0; i < s_vk.framesInFlight; ++i)
    {
        if (!createHostBuffer(static_cast<VkDeviceSize>(sizeBytes), usage, buf.copies[i]))
        {
            for (uint32_t j = 0; j <= i; ++j)
            {
                if (buf.copies[j].mapped && buf.copies[j].memory != VK_NULL_HANDLE)
                    vkUnmapMemory(s_vk.device, buf.copies[j].memory);
                if (buf.copies[j].buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(s_vk.device, buf.copies[j].buffer, nullptr);
                if (buf.copies[j].memory != VK_NULL_HANDLE)
                    vkFreeMemory(s_vk.device, buf.copies[j].memory, nullptr);
            }
            return 0;
        }
        if (data)
            std::memcpy(buf.copies[i].mapped, data, static_cast<size_t>(sizeBytes));
    }

    s_vk.totalBufBytes += static_cast<uint64_t>(sizeBytes) * s_vk.framesInFlight;
    if (isIndex)
        return s_vk.ebos.allocate(std::move(buf)); // PERF-RDG4R
    return s_vk.vbos.allocate(std::move(buf));     // PERF-RDG4R
}

void updateDynBuffer(HandleTable<GpuBuffer> &table, uint32_t id, const void *data, int sizeBytes, int offsetBytes)
{
    if (!data || sizeBytes <= 0 || offsetBytes < 0)
        return;
    GpuBuffer *bufPtr = table.find(id);
    if (!bufPtr)
        return;
    GpuBuffer &buf = *bufPtr;
    if (offsetBytes + sizeBytes > buf.sizeBytes)
        return;

    const uint32_t cur = s_vk.curFrame;
    if (cur < buf.copies.size() && buf.copies[cur].mapped)
    {
        std::memcpy(static_cast<uint8_t *>(buf.copies[cur].mapped) + offsetBytes, data, static_cast<size_t>(sizeBytes));
    }

    PendingBufferWrite w;
    w.offset = offsetBytes;
    w.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + sizeBytes);
    for (uint32_t i = 0; i < s_vk.framesInFlight; ++i)
    {
        if (i != cur)
            buf.pending[i].push_back(w);
    }
}

void freeDynBuffer(HandleTable<GpuBuffer> &table, uint32_t id)
{
    GpuBuffer *bufPtr = table.find(id);
    if (!bufPtr)
        return;
    GpuBuffer &buf = *bufPtr;
    if (s_vk.totalBufBytes >= static_cast<uint64_t>(buf.sizeBytes) * s_vk.framesInFlight)
        s_vk.totalBufBytes -= static_cast<uint64_t>(buf.sizeBytes) * s_vk.framesInFlight;

    for (auto &c : buf.copies)
    {
        if (c.mapped && c.memory != VK_NULL_HANDLE)
            vkUnmapMemory(s_vk.device, c.memory);
        if (s_vk.frameActive)
            s_vk.garbageBuffers[s_vk.curFrame].push_back({c.buffer, c.memory});
        else
        {
            if (c.buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(s_vk.device, c.buffer, nullptr);
            if (c.memory != VK_NULL_HANDLE)
                vkFreeMemory(s_vk.device, c.memory, nullptr);
        }
    }
    table.erase(id);
}

void flushGarbage(uint32_t frameIdx)
{
    if (frameIdx >= s_vk.framesInFlight)
        return;
    // FIX-V3D: free async upload command buffers
    if (frameIdx < s_vk.garbageCmds.size() && !s_vk.garbageCmds[frameIdx].empty())
    {
        vkFreeCommandBuffers(s_vk.device, s_vk.uploadPool,
                             static_cast<uint32_t>(s_vk.garbageCmds[frameIdx].size()),
                             s_vk.garbageCmds[frameIdx].data());
        s_vk.garbageCmds[frameIdx].clear();
    }

    // FIX-V3M: move this frame's buffer garbage into backlog
    if (!s_vk.garbageBuffers[frameIdx].empty())
    {
        s_vk.garbageBufBacklog.insert(s_vk.garbageBufBacklog.end(),
                                     s_vk.garbageBuffers[frameIdx].begin(),
                                     s_vk.garbageBuffers[frameIdx].end());
        s_vk.garbageBuffers[frameIdx].clear();
    }

    // FIX-V3E: pool recycled texture images instead of destroy/alloc
    const uint64_t MAX_POOL_BYTES = 256ULL * 1024 * 1024;
    for (const auto &gt : s_vk.garbageTextures[frameIdx])
    {
        if (gt.w > 0 && gt.h > 0 && gt.mips > 0 && gt.image != VK_NULL_HANDLE && gt.memory != VK_NULL_HANDLE && gt.view != VK_NULL_HANDLE)
        {
            const uint64_t bytes = gt.bytes > 0 ? gt.bytes : (static_cast<uint64_t>(gt.w) * gt.h * 4);
            while (!s_vk.imagePool.empty() && (s_vk.poolTotalBytes + bytes > MAX_POOL_BYTES))
            {
                auto oldest = s_vk.imagePool.begin();
                for (auto it = s_vk.imagePool.begin(); it != s_vk.imagePool.end(); ++it)
                {
                    if (it->second.lastUsedSerial < oldest->second.lastUsedSerial)
                        oldest = it;
                }
                // FIX-V3M: push evicted image into backlog instead of destroying inline
                s_vk.garbageTexBacklog.push_back({oldest->second.view, oldest->second.image, oldest->second.memory, 0, 0, 0, 0});
                if (s_vk.poolTotalBytes >= oldest->second.bytes)
                    s_vk.poolTotalBytes -= oldest->second.bytes;
                else
                    s_vk.poolTotalBytes = 0;
                s_vk.imagePool.erase(oldest);
            }

            if (s_vk.poolTotalBytes + bytes <= MAX_POOL_BYTES)
            {
                PooledImage pi;
                pi.image = gt.image;
                pi.memory = gt.memory;
                pi.view = gt.view;
                pi.w = gt.w;
                pi.h = gt.h;
                pi.mips = gt.mips;
                pi.bytes = bytes;
                pi.lastUsedSerial = ++s_vk.poolSerial;
                s_vk.poolTotalBytes += bytes;
                s_vk.imagePool.emplace(imagePoolKey(gt.w, gt.h, gt.mips), pi);
                continue;
            }
        }

        // FIX-V3M: push unpooled texture garbage to backlog instead of destroying inline
        s_vk.garbageTexBacklog.push_back(gt);
    }
    s_vk.garbageTextures[frameIdx].clear();

    const size_t totalBacklog = s_vk.garbageBufBacklog.size() + s_vk.garbageTexBacklog.size(); // FIX-V3M
    if (totalBacklog > s_vk.statBacklogMax) // FIX-V3M
        s_vk.statBacklogMax = totalBacklog;

    // FIX-V3M: process backlog FIFO with budget of 1.0 ms, checking clock every 4 items
    const auto tBudgetStart = std::chrono::steady_clock::now();
    const double budgetMs = 1.0;
    size_t processedCount = 0;

    size_t bufProcessed = 0;
    while (bufProcessed < s_vk.garbageBufBacklog.size())
    {
        const auto &gb = s_vk.garbageBufBacklog[bufProcessed];
        if (gb.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(s_vk.device, gb.buffer, nullptr);
        if (gb.memory != VK_NULL_HANDLE)
            vkFreeMemory(s_vk.device, gb.memory, nullptr);
        bufProcessed++;
        processedCount++;

        if (processedCount >= 4 && (processedCount % 4) == 0)
        {
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBudgetStart).count() >= budgetMs)
                break;
        }
    }
    if (bufProcessed > 0)
    {
        s_vk.garbageBufBacklog.erase(s_vk.garbageBufBacklog.begin(), s_vk.garbageBufBacklog.begin() + bufProcessed);
    }

    if (s_vk.garbageBufBacklog.empty() &&
        (processedCount < 4 || std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBudgetStart).count() < budgetMs))
    {
        size_t texProcessed = 0;
        while (texProcessed < s_vk.garbageTexBacklog.size())
        {
            const auto &gt = s_vk.garbageTexBacklog[texProcessed];
            if (gt.view != VK_NULL_HANDLE)
                vkDestroyImageView(s_vk.device, gt.view, nullptr);
            if (gt.image != VK_NULL_HANDLE)
                vkDestroyImage(s_vk.device, gt.image, nullptr);
            if (gt.memory != VK_NULL_HANDLE)
                vkFreeMemory(s_vk.device, gt.memory, nullptr);
            texProcessed++;
            processedCount++;

            if (processedCount >= 4 && (processedCount % 4) == 0)
            {
                if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBudgetStart).count() >= budgetMs)
                    break;
            }
        }
        if (texProcessed > 0)
        {
            s_vk.garbageTexBacklog.erase(s_vk.garbageTexBacklog.begin(), s_vk.garbageTexBacklog.begin() + texProcessed);
        }
    }
}

void applyPendingBufferWrites(uint32_t frameIdx)
{
    auto applyTable = [frameIdx](HandleTable<GpuBuffer> &table) // PERF-RDG4R
    {
        for (auto &slot : table.slots)
        {
            if (!slot.alive)
                continue;
            GpuBuffer &buf = slot.value;
            if (frameIdx >= buf.pending.size() || buf.pending[frameIdx].empty())
                continue;
            uint8_t *dst = static_cast<uint8_t *>(buf.copies[frameIdx].mapped);
            for (const auto &pw : buf.pending[frameIdx])
            {
                std::memcpy(dst + pw.offset, pw.data.data(), pw.data.size());
            }
            buf.pending[frameIdx].clear();
        }
    };
    applyTable(s_vk.vbos);
    applyTable(s_vk.ebos);
}

// FIX-RDG3C
VkDescriptorPool createDescriptorPool()
{
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384u * 2u};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 16384u;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(s_vk.device, &dpci, nullptr, &pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pool;
}

bool allocDescriptorSet(uint32_t frameIdx, VkDescriptorSet &outDs)
{
    if (frameIdx >= s_vk.descPools.size() || s_vk.descPools[frameIdx].empty())
        return false;

    auto tryAlloc = [&](VkDescriptorPool pool, VkResult &outRes) -> bool
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &s_vk.descSetLayout;
        outRes = vkAllocateDescriptorSets(s_vk.device, &dsai, &outDs);
        return outRes == VK_SUCCESS;
    };

    size_t &idx = s_vk.descPoolIdx[frameIdx];
    VkResult res = VK_SUCCESS;
    if (tryAlloc(s_vk.descPools[frameIdx][idx], res))
        return true;

    if (!s_vk.descAllocFailLogged)
    {
        s_vk.descAllocFailLogged = true;
        std::printf("[rdvk] descAlloc failed: %d\n", static_cast<int>(res));
    }

    ++idx;
    if (idx >= s_vk.descPools[frameIdx].size())
    {
        VkDescriptorPool newPool = createDescriptorPool();
        if (newPool == VK_NULL_HANDLE)
            return false;
        s_vk.descPools[frameIdx].push_back(newPool);
    }
    return tryAlloc(s_vk.descPools[frameIdx][idx], res);
}

uint32_t resolveBoundVbo(VkDeviceSize *outOffset = nullptr)
{
    if (outOffset)
        *outOffset = 0;
    if (s_vk.boundVao != 0)
    {
        if (const GpuVao *vao = s_vk.vaos.find(s_vk.boundVao); vao && vao->vbo != 0) // PERF-RDG4R
        {
            if (outOffset)
                *outOffset = vao->vbOffset;
            return vao->vbo;
        }
    }
    return s_vk.boundVbo;
}

uint32_t resolveBoundEbo()
{
    if (s_vk.boundVao != 0)
    {
        if (const GpuVao *vao = s_vk.vaos.find(s_vk.boundVao); vao && vao->ebo != 0) // PERF-RDG4R
            return vao->ebo;
    }
    return s_vk.boundEbo;
}

// FEAT-ENH2
void enhTrackDraw(uint32_t count)
{
    const PushConstants &pc = s_vk.pc;
    if (pc.uMode2d != 0 || !(pc.uVpB[3] > 0.5f) || count <= s_vk.enh3dCount || !s_vk.lastVpValid)
        return;
    s_vk.enh3dCount = count;
    std::memcpy(s_vk.enhFlip, pc.uFlip, sizeof(s_vk.enhFlip));
    std::memcpy(s_vk.enhVpA, pc.uVpA, sizeof(s_vk.enhVpA));
    std::memcpy(s_vk.enhVpB, pc.uVpB, sizeof(s_vk.enhVpB));
    std::memcpy(s_vk.enhFbSize, pc.uFbSize, sizeof(s_vk.enhFbSize));
    s_vk.enhZBias = pc.uZBias;
    s_vk.enh3dViewport = s_vk.lastVp;
}

bool prepareDraw()
{
    ProbeDrawScope drawProbe; // PROBE-RDG4P
    const bool fg5On = fg5ProfActive(); // PROBE-FG5 // FEAT-FG6
    const int64_t fg5PrepT0 = fg5On ? probeQpcNow() : 0; // PROBE-FG5
    int64_t fg5UniformTicks = 0; // PROBE-FG5
    uint64_t fg5StateChanges = 0; // PROBE-FG5
    uint64_t fg5UniformUpdates = 0; // PROBE-FG5
    int64_t fg9PipeLookT = 0, fg9BindPipeT = 0, fg9BindDescT = 0, fg9BindVbT = 0, fg9DynStateT = 0; // PROBE-FG9
    uint64_t fg9PipeLookC = 0, fg9PipeMissC = 0, fg9BindPipeC = 0, fg9BindDescC = 0, fg9BindVbC = 0, fg9DynStateC = 0; // PROBE-FG9
    if (!s_vk.frameActive || !s_vk.shaderReady || s_vk.cmd == VK_NULL_HANDLE)
    {
        ++s_vk.statNoFrame; // PROBE-RDG3B
        return false;
    }

    ensureRenderPass(false, 0.0f, 0.0f, 0.0f, 1.0f);
    if (!s_vk.inRenderPass)
    {
        ++s_vk.statNoPass; // PROBE-RDG3B
        return false;
    }

    // PERF-RDG4R: only re-resolve pipeline when pipeKey / msaaSamples changed.
    const uint8_t curSamples = static_cast<uint8_t>(s_vk.msaaSamples); // RDG2B
    if (s_vk.pipeKey.samples != curSamples)
    {
        s_vk.pipeKey.samples = curSamples;
        s_vk.dirtyPipeline = true;
    }
    if (s_vk.dirtyPipeline || s_vk.cachedPipeline == VK_NULL_HANDLE)
    {
        const int64_t t0 = fg5On ? probeQpcNow() : 0; // PROBE-FG9
        bool pipeMiss = false; // PROBE-FG9
        s_vk.cachedPipeline = getOrCreatePipeline(s_vk.pipeKey, fg5On ? &pipeMiss : nullptr);
        s_vk.dirtyPipeline = false;
        if (fg5On) // PROBE-FG9
        {
            fg9PipeLookT += (probeQpcNow() - t0);
            ++fg9PipeLookC;
            if (pipeMiss) ++fg9PipeMissC;
        }
    }
    if (s_vk.cachedPipeline == VK_NULL_HANDLE)
    {
        ++s_vk.statNoPipeline; // PROBE-RDG3B
        return false;
    }
    if (s_vk.cachedPipeline != s_vk.boundPipeline)
    {
        const int64_t t0 = fg5On ? probeQpcNow() : 0; // PROBE-FG9
        vkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.cachedPipeline);
        s_vk.boundPipeline = s_vk.cachedPipeline;
        if (fg5On) // PROBE-FG5 / PROBE-FG9
        {
            ++fg5StateChanges;
            fg9BindPipeT += (probeQpcNow() - t0);
            ++fg9BindPipeC;
        }
    }

    // PERF-RDG4R: only emit vkCmdSetViewport / vkCmdSetScissor when effective state changed.
    if (fg5On && (s_vk.dirtyViewport || s_vk.dirtyScissor)) ++fg5StateChanges; // PROBE-FG5
    {
        const int64_t t0 = fg5On ? probeQpcNow() : 0; // PROBE-FG9
        applyViewportAndScissorIfDirty(fg5On ? &fg9DynStateC : nullptr);
        if (fg5On) fg9DynStateT += (probeQpcNow() - t0); // PROBE-FG9
    }

    // FIX-RDG3C / PERF-RDG4R: only resolve textures, samplers, and hash DescCacheKey when dirty.
    const int64_t fg9DescT0 = fg5On ? probeQpcNow() : 0; // PROBE-FG9
    if (s_vk.dirtyTextures || s_vk.cachedDescSet == VK_NULL_HANDLE ||
        !s_vk.cachedTex[0] || !s_vk.cachedTex[1])
    {
        auto resolveTex = [](uint32_t id, uint32_t &outId) -> const GpuTexture *
        {
            if (const GpuTexture *t = s_vk.textures.find(id))
            {
                outId = id;
                return t;
            }
            if (const GpuTexture *def = s_vk.textures.find(s_vk.defaultWhiteTex))
            {
                outId = s_vk.defaultWhiteTex;
                return def;
            }
            outId = 0;
            return nullptr;
        };

        uint32_t effId0 = 0, effId1 = 0;
        const GpuTexture *tex0 = resolveTex(s_vk.boundTex[0], effId0);
        const GpuTexture *tex1 = resolveTex(s_vk.boundTex[1], effId1);
        if (!tex0 || !tex1 || tex0->view == VK_NULL_HANDLE || tex1->view == VK_NULL_HANDLE)
        {
            s_vk.cachedDescSet = VK_NULL_HANDLE;
            ++s_vk.statNoDesc; // PROBE-RDG3B
            return false;
        }

        VkSampler samp0 = getOrCreateSampler(tex0->sampler);
        VkSampler samp1 = getOrCreateSampler(tex1->sampler);
        if (samp0 == VK_NULL_HANDLE || samp1 == VK_NULL_HANDLE)
        {
            s_vk.cachedDescSet = VK_NULL_HANDLE;
            ++s_vk.statNoDesc; // PROBE-RDG3B
            return false;
        }

        s_vk.cachedTex[0] = tex0;
        s_vk.cachedTex[1] = tex1;
        s_vk.cachedEffTexId[0] = effId0;
        s_vk.cachedEffTexId[1] = effId1;
        s_vk.cachedSampler[0] = samp0;
        s_vk.cachedSampler[1] = samp1;

        DescCacheKey cacheKey{effId0, effId1, tex0->sampler.pack(), tex1->sampler.pack(), 0}; // FIX-FG9
        cacheKey.computeHash(); // FIX-FG9
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (fg6StateCacheEnabled() && s_vk.lastDescCacheFrame == s_vk.curFrame &&
            s_vk.lastDescCacheSet != VK_NULL_HANDLE && cacheKey == s_vk.lastDescKey) // FIX-FG9
        {
            ds = s_vk.lastDescCacheSet;
        }
        else
        {
            auto &cache = s_vk.descCaches[s_vk.curFrame];
            if (auto cit = cache.find(cacheKey); cit != cache.end())
            {
                ds = cit->second;
                if (s_vk.logEnabled) // PROBE-RDG4P
                    ++s_vk.curFrameDescHits;
            }
            else
            {
                if (!allocDescriptorSet(s_vk.curFrame, ds))
                {
                    s_vk.cachedDescSet = VK_NULL_HANDLE;
                    ++s_vk.statNoDesc; // PROBE-RDG3B
                    return false;
                }
                if (s_vk.logEnabled) // PROBE-RDG4P
                    ++s_vk.curFrameDescMisses;

                VkDescriptorImageInfo imgInfos[2]{};
                imgInfos[0] = {samp0, tex0->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                imgInfos[1] = {samp1, tex1->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

                VkWriteDescriptorSet writes[2]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = ds;
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].pImageInfo = &imgInfos[0];

                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = ds;
                writes[1].dstBinding = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &imgInfos[1];

                vkUpdateDescriptorSets(s_vk.device, 2, writes, 0, nullptr);
                cache.emplace(cacheKey, ds);
            }
            if (fg6StateCacheEnabled()) // FIX-FG9
            {
                s_vk.lastDescKey = cacheKey;
                s_vk.lastDescCacheSet = ds;
                s_vk.lastDescCacheFrame = s_vk.curFrame;
            }
        }

        s_vk.cachedDescSet = ds;
        s_vk.dirtyTextures = false;
    }

    if (s_vk.cachedDescSet != s_vk.lastBoundDescSet)
    {
        vkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipelineLayout, 0, 1, &s_vk.cachedDescSet, 0, nullptr);
        s_vk.lastBoundDescSet = s_vk.cachedDescSet;
        if (fg5On) // PROBE-FG5 / PROBE-FG9
        {
            ++fg5StateChanges;
            ++fg9BindDescC;
        }
    }
    if (fg5On) fg9BindDescT += (probeQpcNow() - fg9DescT0); // PROBE-FG9

    // PERF-RDG4R / FIX-FG7: only push the changed 4-byte-aligned push-constant span.
    if (s_vk.dirtyPushConstants || !s_vk.lastPcValid)
    {
        uint32_t byteOff = 0u;
        uint32_t byteSize = 0u;
        if (!s_vk.lastPcValid)
        {
            byteOff = 0u;
            byteSize = static_cast<uint32_t>(sizeof(PushConstants));
        }
        else
        {
            const uint32_t *curW = reinterpret_cast<const uint32_t *>(&s_vk.pc);
            const uint32_t *oldW = reinterpret_cast<const uint32_t *>(&s_vk.lastPc);
            constexpr uint32_t kWords = static_cast<uint32_t>(sizeof(PushConstants) / 4u);
            uint32_t firstW = kWords;
            uint32_t lastW = 0u;
            for (uint32_t w = 0u; w < kWords; ++w)
            {
                if (curW[w] != oldW[w])
                {
                    if (firstW == kWords) firstW = w;
                    lastW = w;
                    if (fg5On) ps2xRdDrawProfAddFg8PcWord(w, oldW[w], curW[w]); // PROBE-FG8
                }
            }
            if (firstW < kWords)
            {
                byteOff = firstW * 4u;
                byteSize = (lastW - firstW + 1u) * 4u;
            }
        }
        if (byteSize > 0u)
        {
            const int64_t fg5UT0 = fg5On ? probeQpcNow() : 0; // PROBE-FG5
            vkCmdPushConstants(s_vk.cmd, s_vk.pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               byteOff, byteSize, reinterpret_cast<const uint8_t *>(&s_vk.pc) + byteOff);
            if (fg5On) // PROBE-FG5 / PROBE-FG7
            {
                fg5UniformTicks += (probeQpcNow() - fg5UT0);
                ++fg5UniformUpdates;
                ps2xRdDrawProfAddFg7Pc(1, byteSize);
            }
            s_vk.lastPc = s_vk.pc;
            s_vk.lastPcValid = true;
        }
        s_vk.dirtyPushConstants = false;
    }

    // FIX-RDG3C / PERF-RDG4R: resolve VBO only when vertex binding is dirty.
    if (s_vk.dirtyVertexBuffer || s_vk.lastBoundVb == VK_NULL_HANDLE)
    {
        const int64_t t0 = fg5On ? probeQpcNow() : 0; // PROBE-FG9
        VkDeviceSize offset = 0;
        const uint32_t activeVbo = resolveBoundVbo(&offset);
        const GpuBuffer *vbo = s_vk.vbos.find(activeVbo);
        if (!vbo)
        {
            ++s_vk.statNoVb; // PROBE-RDG3B
            return false;
        }
        VkBuffer vb = vbo->copies[s_vk.curFrame].buffer;
        if (vb != s_vk.lastBoundVb || offset != s_vk.lastBoundVbOffset)
        {
            vkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vb, &offset);
            s_vk.lastBoundVb = vb;
            s_vk.lastBoundVbOffset = offset;
            if (fg5On) // PROBE-FG5 / PROBE-FG9
            {
                ++fg5StateChanges;
                ++fg9BindVbC;
            }
        }
        s_vk.dirtyVertexBuffer = false;
        if (fg5On) fg9BindVbT += (probeQpcNow() - t0); // PROBE-FG9
    }

    ++s_vk.statDraws;
    if (s_vk.logEnabled) // PROBE-RDG4P
        ++s_vk.curFrameDraws;
    if (fg5On) // PROBE-FG5 / PROBE-FG9
    {
        const int64_t prepTicks = (probeQpcNow() - fg5PrepT0) - fg5UniformTicks;
        ps2xRdDrawProfAddVk(prepTicks > 0 ? prepTicks : 0, fg5UniformTicks, 0, fg5StateChanges, fg5UniformUpdates, 0);
        ps2xRdDrawProfAddFg9Bind(fg9PipeLookT, fg9PipeLookC, fg9PipeMissC,
                                 fg9BindPipeT, fg9BindPipeC,
                                 fg9BindDescT, fg9BindDescC,
                                 fg9BindVbT, fg9BindVbC,
                                 fg9DynStateT, fg9DynStateC,
                                 fg5UniformTicks, fg5UniformUpdates);
    }
    return true;
}

} // namespace

bool active()
{
    return s_vk.initialized;
}

bool init()
{
    if (s_vk.initialized)
        return true;

    s_vk.device = ps2xSdlVkDevice();
    s_vk.physicalDevice = ps2xSdlVkPhysicalDevice();
    s_vk.queue = ps2xSdlVkQueue();
    s_vk.queueFamily = ps2xSdlVkQueueFamily();
    s_vk.framesInFlight = std::max(1u, ps2xSdlVkFramesInFlight());

    if (s_vk.device == VK_NULL_HANDLE || s_vk.physicalDevice == VK_NULL_HANDLE || s_vk.queue == VK_NULL_HANDLE)
        return false;

    const std::filesystem::path shaderDir = getExeDir() / "shaders";
    s_vk.vertModule = createShaderModuleFromFile(shaderDir / "rd.vert.spv");
    s_vk.fragModule = createShaderModuleFromFile(shaderDir / "rd.frag.spv");
    if (s_vk.vertModule == VK_NULL_HANDLE || s_vk.fragModule == VK_NULL_HANDLE)
    {
        std::printf("[rdvk] shader load failed\n");
        if (s_vk.vertModule != VK_NULL_HANDLE) { vkDestroyShaderModule(s_vk.device, s_vk.vertModule, nullptr); s_vk.vertModule = VK_NULL_HANDLE; }
        if (s_vk.fragModule != VK_NULL_HANDLE) { vkDestroyShaderModule(s_vk.device, s_vk.fragModule, nullptr); s_vk.fragModule = VK_NULL_HANDLE; }
        return false;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = s_vk.queueFamily;
    if (vkCreateCommandPool(s_vk.device, &cpci, nullptr, &s_vk.uploadPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s_vk.device, &dslci, nullptr, &s_vk.descSetLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s_vk.descSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(s_vk.device, &plci, nullptr, &s_vk.pipelineLayout) != VK_SUCCESS)
        return false;

    // FIX-RDG3C
    s_vk.descPools.resize(s_vk.framesInFlight);
    s_vk.descPoolIdx.assign(s_vk.framesInFlight, 0);
    s_vk.descCaches.resize(s_vk.framesInFlight);
    for (uint32_t i = 0; i < s_vk.framesInFlight; ++i)
    {
        VkDescriptorPool pool = createDescriptorPool();
        if (pool == VK_NULL_HANDLE)
            return false;
        s_vk.descPools[i].push_back(pool);
    }

    s_vk.garbageBuffers.resize(s_vk.framesInFlight);
    s_vk.garbageTextures.resize(s_vk.framesInFlight);
    s_vk.garbageCmds.resize(s_vk.framesInFlight); // FIX-V3D
    s_vk.frameUploadCmd.assign(s_vk.framesInFlight, VK_NULL_HANDLE); // FIX-V3G
    s_vk.stagingRings.resize(s_vk.framesInFlight); // FIX-V3D
    s_vk.stagingCursors.assign(s_vk.framesInFlight, 0); // FIX-V3D

    // RDG2B: MSAA setup (PS2X_MSAA >= 2 -> 4x)
    s_vk.msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    if (const char *msaaEnv = std::getenv("PS2X_MSAA"))
    {
        if (std::atoi(msaaEnv) >= 2)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(s_vk.physicalDevice, &props);
            const VkSampleCountFlags supported =
                props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
            if (supported & VK_SAMPLE_COUNT_4_BIT)
                s_vk.msaaSamples = VK_SAMPLE_COUNT_4_BIT;
            else
                std::printf("[rdvk] PS2X_MSAA=4x not supported by device, falling back to 1x\n");
        }
    }

    // RDG2B: per-frame palette state
    s_vk.palTexIds.assign(s_vk.framesInFlight, 0u);
    s_vk.palLastRgba.assign(s_vk.framesInFlight, std::vector<uint32_t>(256u, 0u));
    s_vk.palHasData.assign(s_vk.framesInFlight, false);

    s_vk.pc = PushConstants{};
    s_vk.pipeKey.samples = static_cast<uint8_t>(s_vk.msaaSamples); // RDG2B
    s_vk.pc.uFlip[0] = 1.0f;
    s_vk.pc.uFlip[1] = 1.0f;
    s_vk.pc.uFlip[2] = 1.0f;
    s_vk.pc.uFixAlpha = -1.0f;
    s_vk.pc.uAtst = 1;

    if (const char *logEnv = std::getenv("PS2X_RDVK_LOG"))
        s_vk.logEnabled = (logEnv[0] == '1');
    s_vk.lastLogTime = std::chrono::steady_clock::now();

    s_vk.initialized = true;
    s_vk.shaderReady = true;

    const uint32_t whitePixel = 0xFFFFFFFFu;
    s_vk.defaultWhiteTex = rdgfx_vk::createTextureRGBA8(&whitePixel, 1, 1);

    return true;
}

// FIX-R2: render scale support
static int s_renderScalePercent = -1;

int getRenderScalePercent()
{
    if (s_renderScalePercent < 0)
    {
        s_renderScalePercent = 100;
        if (const char *env = std::getenv("PS2X_RENDER_SCALE"))
        {
            char *end = nullptr;
            long val = std::strtol(env, &end, 10);
            if (end != env)
            {
                while (*end == ' ' || *end == '\t') ++end;
                if (*end == '%') ++end;
                while (*end == ' ' || *end == '\t') ++end;
                if (*end == '\0')
                {
                    if (val < 25) val = 25;
                    if (val > 400) val = 400;
                    s_renderScalePercent = static_cast<int>(val);
                }
            }
        }
    }
    return s_renderScalePercent;
}

void setRenderScalePercent(int percent)
{
    if (percent < 25) percent = 25;
    if (percent > 400) percent = 400;
    s_renderScalePercent = percent;
}

void beginFrame()
{
    if (!s_vk.initialized)
        return;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage swapImg = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, frameIdx = 0;
    if (!ps2xSdlVkBeginRdFrame(&cmd, &swapImg, &w, &h, &frameIdx))
    {
        s_vk.frameActive = false;
        return;
    }

    if (s_vk.logEnabled) // PROBE-RDG4P
    {
        s_vk.curFrameWaitMs = ps2xSdlVkLastBeginWaitMs();
        s_vk.curFrameRecMs = 0.0;
        s_vk.curFrameDrawTicks = 0;
        s_vk.curFrameTexTicks = 0;
        s_vk.curFrameEndMs = 0.0;
        s_vk.curFrameDraws = 0;
        s_vk.curFrameDescHits = 0;
        s_vk.curFrameDescMisses = 0;
    }

    enhanceVerifyCollect(frameIdx); // PROBE-ENH1: this slot's fence has completed

    s_vk.cmd = cmd;
    s_vk.swapImage = swapImg;
    s_vk.swapW = w; // FIX-R2
    s_vk.swapH = h; // FIX-R2
    s_vk.curFrame = frameIdx;
    s_vk.inRenderPass = false;
    s_vk.firstPassInFrame = true;
    s_vk.enhTrack = ps2xEnhanceOn() && ps2xEnhanceDebugView() != 0; // FEAT-ENH2
    s_vk.enh3dCount = 0; // FEAT-ENH2
    invalidateCommandBufferState(); // PERF-RDG4R
    s_vk.cachedDescSet = VK_NULL_HANDLE;

    // FIX-R2: compute scaled target size
    const int scalePct = getRenderScalePercent();
    const double scale = static_cast<double>(scalePct) / 100.0;
    uint32_t targetW = static_cast<uint32_t>(std::max(64.0, std::round(static_cast<double>(w) * scale)));
    uint32_t targetH = static_cast<uint32_t>(std::max(64.0, std::round(static_cast<double>(h) * scale)));

    VkPhysicalDeviceProperties devProps{};
    vkGetPhysicalDeviceProperties(s_vk.physicalDevice, &devProps);
    const uint32_t maxDim = devProps.limits.maxImageDimension2D;
    double factor = 1.0;
    if (maxDim > 0 && targetW > maxDim)
        factor = std::min(factor, static_cast<double>(maxDim) / static_cast<double>(targetW));
    if (maxDim > 0 && targetH > maxDim)
        factor = std::min(factor, static_cast<double>(maxDim) / static_cast<double>(targetH));
    const double pixelBudget = 8192.0 * 8192.0;
    const double currentPixels = static_cast<double>(targetW) * static_cast<double>(targetH);
    if (currentPixels > pixelBudget)
        factor = std::min(factor, std::sqrt(pixelBudget / currentPixels));

    if (factor < 1.0)
    {
        targetW = static_cast<uint32_t>(std::max(64.0, std::floor(static_cast<double>(targetW) * factor)));
        targetH = static_cast<uint32_t>(std::max(64.0, std::floor(static_cast<double>(targetH) * factor)));
        const int clampedPct = static_cast<int>(std::round((static_cast<double>(targetW) / static_cast<double>(w)) * 100.0));
        static bool s_clampedLogged = false;
        if (!s_clampedLogged)
        {
            s_clampedLogged = true;
            std::printf("[rdvk] render scale %d%% clamped to %d%%\n", scalePct, clampedPct);
        }
    }

    static bool s_initScaleLogged = false;
    if (!s_initScaleLogged)
    {
        s_initScaleLogged = true;
        std::printf("[rdvk] render scale=%d%% target %ux%u swapchain %ux%u\n",
                    scalePct, targetW, targetH, w, h);
    }

    if (!ensureTargets(targetW, targetH))
    {
        s_vk.frameActive = false;
        return;
    }

    // FIX-RDG3C: reset all per-frame descriptor pools and clear per-frame cache after the frame fence.
    for (VkDescriptorPool pool : s_vk.descPools[frameIdx])
        vkResetDescriptorPool(s_vk.device, pool, 0);
    s_vk.descPoolIdx[frameIdx] = 0;
    s_vk.descCaches[frameIdx].clear();
    if (s_vk.lastDescCacheFrame == frameIdx) // FIX-FG9
    {
        s_vk.lastDescCacheSet = VK_NULL_HANDLE;
        s_vk.lastDescCacheFrame = UINT32_MAX;
    }
    // FIX-V3G: safe discard unsubmitted batch cmds if frame was aborted
    for (uint32_t i = 0; i < s_vk.frameUploadCmd.size(); ++i)
    {
        if (s_vk.frameUploadCmd[i] != VK_NULL_HANDLE)
        {
            vkEndCommandBuffer(s_vk.frameUploadCmd[i]);
            s_vk.garbageCmds[i].push_back(s_vk.frameUploadCmd[i]);
            s_vk.frameUploadCmd[i] = VK_NULL_HANDLE;
        }
    }
    flushGarbage(frameIdx);
    s_vk.stagingCursors[frameIdx] = 0; // FIX-V3D
    applyPendingBufferWrites(frameIdx);
    s_vk.overlayVertexCursor = 0; // RDG2B

    s_vk.frameActive = true;
    if (s_vk.logEnabled) // PROBE-RDG4P
        s_vk.recStartQpc = probeQpcNow();
}

void endFrame()
{
    if (!s_vk.initialized || !s_vk.frameActive)
        return;

    // FIX-V3G: submit batched upload commands before main frame command buffer
    if (s_vk.curFrame < s_vk.frameUploadCmd.size() && s_vk.frameUploadCmd[s_vk.curFrame] != VK_NULL_HANDLE)
    {
        VkCommandBuffer ucmd = s_vk.frameUploadCmd[s_vk.curFrame];
        vkEndCommandBuffer(ucmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &ucmd;
        vkQueueSubmit(s_vk.queue, 1, &si, VK_NULL_HANDLE);
        probe_v3b::cntAsyncUpload++;
        s_vk.garbageCmds[s_vk.curFrame].push_back(ucmd);
        s_vk.frameUploadCmd[s_vk.curFrame] = VK_NULL_HANDLE;
    }

    int64_t endStartQpc = 0; // PROBE-RDG4P
    if (s_vk.logEnabled)
    {
        endStartQpc = probeQpcNow();
        s_vk.curFrameRecMs = (s_vk.recStartQpc != 0) ? probeQpcToMs(endStartQpc - s_vk.recStartQpc) : 0.0;
    }

    if (s_vk.inRenderPass)
    {
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
        invalidateCommandBufferState(); // PERF-RDG4R
    }
    else if (s_vk.firstPassInFrame)
    {
        ensureRenderPass(true, 0.0f, 0.0f, 0.0f, 1.0f);
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
        invalidateCommandBufferState(); // PERF-RDG4R
    }

    const bool useMsaa = (s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT && s_vk.resolveImage != VK_NULL_HANDLE); // RDG2B
    VkImage srcImage = useMsaa ? s_vk.resolveImage : s_vk.colorImage;
    VkImageLayout &srcLayout = useMsaa ? s_vk.resolveLayout : s_vk.colorLayout;

    imageBarrier(s_vk.cmd, srcImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    imageBarrier(s_vk.cmd, s_vk.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // FIX-R2b: guard against zero-extent blit before swapchain exists
    if (s_vk.swapW != 0 && s_vk.swapH != 0 && s_vk.targetW != 0 && s_vk.targetH != 0)
    {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(s_vk.targetW), static_cast<int32_t>(s_vk.targetH), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(s_vk.swapW), static_cast<int32_t>(s_vk.swapH), 1}; // FIX-R2
        const VkFilter blitFilter = (s_vk.targetW == s_vk.swapW && s_vk.targetH == s_vk.swapH) // FIX-R2
                                        ? VK_FILTER_NEAREST
                                        : VK_FILTER_LINEAR;
        vkCmdBlitImage(s_vk.cmd,
                       srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_vk.swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, blitFilter); // FIX-R2
    }

#if defined(PS2X_HAS_IMGUI) // DBG1
    ps2xSdlImGuiRecord(s_vk.cmd, s_vk.swapImage, ps2xSdlVkCurrentSwapView(), s_vk.swapW, s_vk.swapH); // FIX-R2
#endif // DBG1

    imageBarrier(s_vk.cmd, s_vk.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    ps2xSdlVkEndRdFrame();
    s_vk.frameActive = false;

    ++s_vk.statFrames;
    if (s_vk.logEnabled)
    {
        // PROBE-RDG4P
        s_vk.curFrameEndMs = probeQpcToMs(probeQpcNow() - endStartQpc);
        const double curDrawMs = probeQpcToMs(s_vk.curFrameDrawTicks);
        const double curTexMs = probeQpcToMs(s_vk.curFrameTexTicks);

        ++s_vk.winFrames;
        s_vk.winWaitSumMs += s_vk.curFrameWaitMs;
        s_vk.winWaitMaxMs = std::max(s_vk.winWaitMaxMs, s_vk.curFrameWaitMs);
        s_vk.winRecSumMs += s_vk.curFrameRecMs;
        s_vk.winRecMaxMs = std::max(s_vk.winRecMaxMs, s_vk.curFrameRecMs);
        s_vk.winDrawSumMs += curDrawMs;
        s_vk.winDrawMaxMs = std::max(s_vk.winDrawMaxMs, curDrawMs);
        s_vk.winTexSumMs += curTexMs;
        s_vk.winTexMaxMs = std::max(s_vk.winTexMaxMs, curTexMs);
        s_vk.winEndSumMs += s_vk.curFrameEndMs;
        s_vk.winEndMaxMs = std::max(s_vk.winEndMaxMs, s_vk.curFrameEndMs);
        s_vk.winDrawsSum += s_vk.curFrameDraws;
        s_vk.winDrawsMax = std::max(s_vk.winDrawsMax, s_vk.curFrameDraws);
        s_vk.winDescHitsSum += s_vk.curFrameDescHits;
        s_vk.winDescHitsMax = std::max(s_vk.winDescHitsMax, s_vk.curFrameDescHits);
        s_vk.winDescMissesSum += s_vk.curFrameDescMisses;
        s_vk.winDescMissesMax = std::max(s_vk.winDescMissesMax, s_vk.curFrameDescMisses);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - s_vk.lastLogTime).count() >= 1)
        {
            double bufMB = static_cast<double>(s_vk.totalBufBytes) / (1024.0 * 1024.0);
            const double invFrames = s_vk.winFrames > 0 ? (1.0 / static_cast<double>(s_vk.winFrames)) : 0.0; // PROBE-RDG4P
            std::printf("[rdvk] frames=%llu draws=%llu pipelines=%zu bufMB=%.2f"
                        " dIdx=%llu dArr=%llu noFrame=%llu noPass=%llu noPipeline=%llu noVb=%llu noIb=%llu noDesc=%llu zeroCount=%llu"
                        " clears=%llu binds=%llu texCreated=%llu vbUpd=%llu uniformSets=%llu passBegun=%llu asyncUp=%u"
                        " t_wait=%.2f/%.2f t_rec=%.2f/%.2f t_draw=%.2f/%.2f t_tex=%.2f/%.2f t_end=%.2f/%.2f"
                        " draws/f=%.1f/%llu descSets/f=%.1f+%.1f(max=%llu+%llu) pool=%llu/%llu backlog=%llu\n", // FIX-V3M
                        static_cast<unsigned long long>(s_vk.statFrames),
                        static_cast<unsigned long long>(s_vk.statDraws),
                        s_vk.pipelines.size(),
                        bufMB,
                        static_cast<unsigned long long>(s_vk.statDrawIdx),
                        static_cast<unsigned long long>(s_vk.statDrawArr),
                        static_cast<unsigned long long>(s_vk.statNoFrame),
                        static_cast<unsigned long long>(s_vk.statNoPass),
                        static_cast<unsigned long long>(s_vk.statNoPipeline),
                        static_cast<unsigned long long>(s_vk.statNoVb),
                        static_cast<unsigned long long>(s_vk.statNoIb),
                        static_cast<unsigned long long>(s_vk.statNoDesc),
                        static_cast<unsigned long long>(s_vk.statZeroCount),
                        static_cast<unsigned long long>(s_vk.statClears),
                        static_cast<unsigned long long>(s_vk.statBinds),
                        static_cast<unsigned long long>(s_vk.statTexCreated),
                        static_cast<unsigned long long>(s_vk.statVbUpd),
                        static_cast<unsigned long long>(s_vk.statUniformSets),
                        static_cast<unsigned long long>(s_vk.statPassBegun),
                        probe_v3b::cntAsyncUpload, // FIX-V3D
                        s_vk.winWaitSumMs * invFrames, s_vk.winWaitMaxMs,
                        s_vk.winRecSumMs * invFrames, s_vk.winRecMaxMs,
                        s_vk.winDrawSumMs * invFrames, s_vk.winDrawMaxMs,
                        s_vk.winTexSumMs * invFrames, s_vk.winTexMaxMs,
                        s_vk.winEndSumMs * invFrames, s_vk.winEndMaxMs,
                        static_cast<double>(s_vk.winDrawsSum) * invFrames,
                        static_cast<unsigned long long>(s_vk.winDrawsMax),
                        static_cast<double>(s_vk.winDescHitsSum) * invFrames,
                        static_cast<double>(s_vk.winDescMissesSum) * invFrames,
                        static_cast<unsigned long long>(s_vk.winDescHitsMax),
                        static_cast<unsigned long long>(s_vk.winDescMissesMax),
                        static_cast<unsigned long long>(s_vk.statTexPoolHit), // FIX-V3E
                        static_cast<unsigned long long>(s_vk.statTexPoolMiss), // FIX-V3E
                        static_cast<unsigned long long>(s_vk.statBacklogMax)); // FIX-V3M
            s_vk.winFrames = 0;
            s_vk.winWaitSumMs = s_vk.winWaitMaxMs = 0.0;
            s_vk.winRecSumMs = s_vk.winRecMaxMs = 0.0;
            s_vk.winDrawSumMs = s_vk.winDrawMaxMs = 0.0;
            s_vk.winTexSumMs = s_vk.winTexMaxMs = 0.0;
            s_vk.winEndSumMs = s_vk.winEndMaxMs = 0.0;
            s_vk.winDrawsSum = s_vk.winDrawsMax = 0;
            s_vk.winDescHitsSum = s_vk.winDescHitsMax = 0;
            s_vk.winDescMissesSum = s_vk.winDescMissesMax = 0;
            s_vk.statBacklogMax = s_vk.garbageBufBacklog.size() + s_vk.garbageTexBacklog.size(); // FIX-V3M
            s_vk.lastLogTime = now;
        }
    }
}

bool initShader() { return init(); }
bool isShaderReady() { return s_vk.shaderReady; }
void enableShader() {}
void disableShader() {}

bool hasUniform(rdgfx::Uniform) { return true; }

void setUniformInt(rdgfx::Uniform u, int v)
{
    if (u == rdgfx::Uniform::AtInvert && fg6StateCacheEnabled()) // FIX-FG8: pass via firstInstance
    {
        if (s_vk.lastPcValid && s_vk.curAtInvert == v)
        {
            if (fg5ProfActive()) ps2xRdDrawProfAddFg6(0, 1);
            return;
        }
        ++s_vk.statUniformSets;
        s_vk.curAtInvert = v;
        return;
    }
    if (fg6StateCacheEnabled() && s_vk.lastPcValid) // FEAT-FG6
    {
        bool same = false;
        switch (u)
        {
        case rdgfx::Uniform::Mode2d: same = (s_vk.pc.uMode2d == v); break;
        case rdgfx::Uniform::Atst: same = (s_vk.pc.uAtst == v); break;
        case rdgfx::Uniform::Aref: same = (s_vk.pc.uAref == v); break;
        case rdgfx::Uniform::AtInvert: same = (s_vk.pc.uAtInvert == v); break;
        case rdgfx::Uniform::PalMode: same = (s_vk.pc.uPalMode == v); break;
        case rdgfx::Uniform::PalFallback: same = (s_vk.pc.uPalFallback == v); break;
        case rdgfx::Uniform::Tex: same = (s_vk.pc.uTex == v); break;
        case rdgfx::Uniform::Tfx: same = (s_vk.pc.uTfx == v); break;
        case rdgfx::Uniform::Tcc: same = (s_vk.pc.uTcc == v); break;
        default: break;
        }
        if (same)
        {
            if (fg5ProfActive()) ps2xRdDrawProfAddFg6(0, 1);
            return;
        }
    }
    ++s_vk.statUniformSets; // PROBE-RDG3B
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8 // PERF-RDG4R
    switch (u)
    {
    case rdgfx::Uniform::Mode2d: s_vk.pc.uMode2d = v; break;
    case rdgfx::Uniform::Atst: s_vk.pc.uAtst = v; break;
    case rdgfx::Uniform::Aref: s_vk.pc.uAref = v; break;
    case rdgfx::Uniform::AtInvert: s_vk.pc.uAtInvert = v; break;
    case rdgfx::Uniform::PalMode: s_vk.pc.uPalMode = v; break;
    case rdgfx::Uniform::PalFallback: s_vk.pc.uPalFallback = v; break;
    case rdgfx::Uniform::Tex: s_vk.pc.uTex = v; break;
    case rdgfx::Uniform::Tfx: s_vk.pc.uTfx = v; break;
    case rdgfx::Uniform::Tcc: s_vk.pc.uTcc = v; break;
    default: break;
    }
}

void setUniformFloat(rdgfx::Uniform u, float v)
{
    if (fg6StateCacheEnabled() && s_vk.lastPcValid) // FEAT-FG6
    {
        bool same = false;
        switch (u)
        {
        case rdgfx::Uniform::ZBias: same = (s_vk.pc.uZBias == v); break;
        case rdgfx::Uniform::FixAlpha: same = (s_vk.pc.uFixAlpha == v); break;
        default: break;
        }
        if (same)
        {
            if (fg5ProfActive()) ps2xRdDrawProfAddFg6(0, 1);
            return;
        }
    }
    ++s_vk.statUniformSets; // PROBE-RDG3B
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8 // PERF-RDG4R
    switch (u)
    {
    case rdgfx::Uniform::ZBias: s_vk.pc.uZBias = v; break;
    case rdgfx::Uniform::FixAlpha: s_vk.pc.uFixAlpha = v; break;
    default: break;
    }
}

void setUniformVec2(rdgfx::Uniform u, const float *v)
{
    if (!v) return;
    if (fg6StateCacheEnabled() && s_vk.lastPcValid) // FEAT-FG6
    {
        bool same = false;
        if (u == rdgfx::Uniform::FbSize)
            same = (s_vk.pc.uFbSize[0] == v[0] && s_vk.pc.uFbSize[1] == v[1]);
        else if (u == rdgfx::Uniform::VpZ)
            same = (s_vk.pc.uVpZ[0] == v[0] && s_vk.pc.uVpZ[1] == v[1]);
        if (same)
        {
            if (fg5ProfActive()) ps2xRdDrawProfAddFg6(0, 1);
            return;
        }
    }
    ++s_vk.statUniformSets; // PROBE-RDG3B
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8 // PERF-RDG4R
    if (u == rdgfx::Uniform::FbSize)
    {
        s_vk.pc.uFbSize[0] = v[0];
        s_vk.pc.uFbSize[1] = v[1];
    }
    else if (u == rdgfx::Uniform::VpZ)
    {
        s_vk.pc.uVpZ[0] = v[0];
        s_vk.pc.uVpZ[1] = v[1];
    }
}

void setUniformVec3Immediate(rdgfx::Uniform u, const float *v)
{
    if (!v) return;
    if (fg6StateCacheEnabled() && s_vk.lastPcValid && u == rdgfx::Uniform::Flip) // FEAT-FG6
    {
        if (s_vk.pc.uFlip[0] == v[0] && s_vk.pc.uFlip[1] == v[1] && s_vk.pc.uFlip[2] == v[2])
        {
            if (fg5ProfActive()) ps2xRdDrawProfAddFg6(0, 1);
            return;
        }
    }
    ++s_vk.statUniformSets; // PROBE-RDG3B
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8 // PERF-RDG4R
    if (u == rdgfx::Uniform::Flip)
    {
        s_vk.pc.uFlip[0] = v[0];
        s_vk.pc.uFlip[1] = v[1];
        s_vk.pc.uFlip[2] = v[2];
        s_vk.pc.uFlip[3] = 0.0f;
    }
}

void setUniformVec4(rdgfx::Uniform u, const float *v)
{
    if (!v) return;
    if (fg6StateCacheEnabled() && s_vk.lastPcValid) // FEAT-FG6
    {
        bool same = false;
        if (u == rdgfx::Uniform::VpA)
            same = (std::memcmp(s_vk.pc.uVpA, v, 4 * sizeof(float)) == 0);
        else if (u == rdgfx::Uniform::VpB)
            same = (std::memcmp(s_vk.pc.uVpB, v, 4 * sizeof(float)) == 0);
        if (same)
        {
            if (fg5ProfActive()) ps2xRdDrawProfAddFg6(0, 1);
            return;
        }
    }
    ++s_vk.statUniformSets; // PROBE-RDG3B
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8 // PERF-RDG4R
    if (u == rdgfx::Uniform::VpA)
        std::memcpy(s_vk.pc.uVpA, v, 4 * sizeof(float));
    else if (u == rdgfx::Uniform::VpB)
        std::memcpy(s_vk.pc.uVpB, v, 4 * sizeof(float));
}

void setFog(float r, float g, float b, float enable) // FIX-E1c
{
    if (s_vk.pc.uFog[0] == r && s_vk.pc.uFog[1] == g && s_vk.pc.uFog[2] == b && s_vk.pc.uFog[3] == enable)
        return;
    s_vk.pc.uFog[0] = r;
    s_vk.pc.uFog[1] = g;
    s_vk.pc.uFog[2] = b;
    s_vk.pc.uFog[3] = enable;
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
}

void flushBatch() {}

void getBackbufferSize(int &rw, int &rh)
{
    rw = static_cast<int>(s_vk.targetW);
    rh = static_cast<int>(s_vk.targetH);
}

void getWindowSize(int &w, int &h) // FIX-RDG3D
{
    w = static_cast<int>(s_vk.targetW);
    h = static_cast<int>(s_vk.targetH);
}

void setViewport(int x, int y, int w, int h)
{
    if (s_vk.vpX != x || s_vk.vpY != y || s_vk.vpW != w || s_vk.vpH != h) // PERF-RDG4R
    {
        s_vk.vpX = x;
        s_vk.vpY = y;
        s_vk.vpW = w;
        s_vk.vpH = h;
        s_vk.dirtyViewport = true;
    }
}

void setScissorTest(bool on)
{
    if (s_vk.scissorTest != on) // PERF-RDG4R
    {
        s_vk.scissorTest = on;
        s_vk.dirtyScissor = true;
    }
}

void setScissorRect(int x, int y, int w, int h)
{
    if (s_vk.scX != x || s_vk.scY != y || s_vk.scW != w || s_vk.scH != h) // PERF-RDG4R
    {
        s_vk.scX = x;
        s_vk.scY = y;
        s_vk.scW = w;
        s_vk.scH = h;
        s_vk.dirtyScissor = true;
    }
}

void setColorMask(bool r, bool g, bool b, bool a)
{
    uint8_t m = 0;
    if (r) m |= VK_COLOR_COMPONENT_R_BIT;
    if (g) m |= VK_COLOR_COMPONENT_G_BIT;
    if (b) m |= VK_COLOR_COMPONENT_B_BIT;
    if (a) m |= VK_COLOR_COMPONENT_A_BIT;
    if (s_vk.pipeKey.colorMask != m) // PERF-RDG4R
    {
        s_vk.pipeKey.colorMask = m;
        s_vk.dirtyPipeline = true;
    }
}

void setDepthTest(bool on)
{
    if (s_vk.pipeKey.depthTest != on) // PERF-RDG4R
    {
        s_vk.pipeKey.depthTest = on;
        s_vk.dirtyPipeline = true;
    }
}
void setDepthMask(bool on)
{
    if (s_vk.pipeKey.depthMask != on) // PERF-RDG4R
    {
        s_vk.pipeKey.depthMask = on;
        s_vk.dirtyPipeline = true;
    }
}
void setDepthFunc(rdgfx::DepthFunc func)
{
    if (s_vk.pipeKey.depthFunc != func) // PERF-RDG4R
    {
        s_vk.pipeKey.depthFunc = func;
        s_vk.dirtyPipeline = true;
    }
}

void setBlendAlpha()
{
    rdgfx_vk::setBlend(true,
             rdgfx::BlendFactor::SrcAlpha, rdgfx::BlendFactor::OneMinusSrcAlpha,
             rdgfx::BlendFactor::SrcAlpha, rdgfx::BlendFactor::OneMinusSrcAlpha,
             rdgfx::BlendEq::Add, rdgfx::BlendEq::Add);
}

void setBlend(bool enable,
              rdgfx::BlendFactor srcRgb,
              rdgfx::BlendFactor dstRgb,
              rdgfx::BlendFactor srcAlpha,
              rdgfx::BlendFactor dstAlpha,
              rdgfx::BlendEq eqRgb,
              rdgfx::BlendEq eqAlpha)
{
    if (s_vk.pipeKey.blendEnable != enable ||
        s_vk.pipeKey.srcRgb != srcRgb ||
        s_vk.pipeKey.dstRgb != dstRgb ||
        s_vk.pipeKey.srcAlpha != srcAlpha ||
        s_vk.pipeKey.dstAlpha != dstAlpha ||
        s_vk.pipeKey.eqRgb != eqRgb ||
        s_vk.pipeKey.eqAlpha != eqAlpha) // PERF-RDG4R
    {
        s_vk.pipeKey.blendEnable = enable;
        s_vk.pipeKey.srcRgb = srcRgb;
        s_vk.pipeKey.dstRgb = dstRgb;
        s_vk.pipeKey.srcAlpha = srcAlpha;
        s_vk.pipeKey.dstAlpha = dstAlpha;
        s_vk.pipeKey.eqRgb = eqRgb;
        s_vk.pipeKey.eqAlpha = eqAlpha;
        s_vk.dirtyPipeline = true;
    }
}

void setBackfaceCulling(bool on)
{
    if (s_vk.pipeKey.cull != on) // PERF-RDG4R
    {
        s_vk.pipeKey.cull = on;
        s_vk.dirtyPipeline = true;
    }
}

void clearScreenBuffers(float r, float g, float b, float a)
{
    ++s_vk.statClears; // PROBE-RDG3B
    if (!s_vk.frameActive)
        return;
    if (!s_vk.inRenderPass)
    {
        ensureRenderPass(true, r, g, b, a);
        return;
    }

    VkClearAttachment atts[2]{};
    atts[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    atts[0].colorAttachment = 0;
    atts[0].clearValue.color.float32[0] = r;
    atts[0].clearValue.color.float32[1] = g;
    atts[0].clearValue.color.float32[2] = b;
    atts[0].clearValue.color.float32[3] = a;

    atts[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    atts[1].clearValue.depthStencil = {1.0f, 0};

    VkClearRect rect{};
    rect.rect = computeVkScissor();
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    if (rect.rect.extent.width > 0 && rect.rect.extent.height > 0)
        vkCmdClearAttachments(s_vk.cmd, 2, atts, 1, &rect);
}

// FIX-RDG3C: emulate GL VAO state (stores VBO on set/enableVertexAttribute and EBO on bindIndexBuffer).
uint32_t createVertexArray()
{
    return s_vk.vaos.allocate(GpuVao{}); // PERF-RDG4R
}

void bindVertexArray(uint32_t vao)
{
    if (s_vk.boundVao != vao) // PERF-RDG4R
    {
        s_vk.boundVao = vao;
        s_vk.dirtyVertexBuffer = true;
        s_vk.dirtyIndexBuffer = true;
    }
}

uint32_t createVertexBuffer(const void *data, int sizeBytes, bool)
{
    return createDynBuffer(data, sizeBytes, false);
}

void updateVertexBuffer(uint32_t vbo, const void *data, int sizeBytes, int offsetBytes)
{
    ++s_vk.statVbUpd; // PROBE-RDG3B
    updateDynBuffer(s_vk.vbos, vbo, data, sizeBytes, offsetBytes);
    s_vk.dirtyVertexBuffer = true; // PERF-RDG4R
}

void freeVertexBuffer(uint32_t vbo)
{
    freeDynBuffer(s_vk.vbos, vbo);
    s_vk.dirtyVertexBuffer = true; // PERF-RDG4R
}

uint32_t createIndexBuffer(const void *data, int sizeBytes, bool)
{
    return createDynBuffer(data, sizeBytes, true);
}

void updateIndexBuffer(uint32_t ebo, const void *data, int sizeBytes, int offsetBytes)
{
    updateDynBuffer(s_vk.ebos, ebo, data, sizeBytes, offsetBytes);
    s_vk.dirtyIndexBuffer = true; // PERF-RDG4R
}

void freeIndexBuffer(uint32_t ebo)
{
    freeDynBuffer(s_vk.ebos, ebo);
    s_vk.dirtyIndexBuffer = true; // PERF-RDG4R
}

void bindVertexBuffer(uint32_t vbo)
{
    if (s_vk.boundVbo != vbo) // PERF-RDG4R
    {
        s_vk.boundVbo = vbo;
        s_vk.dirtyVertexBuffer = true;
    }
}

// FIX-RDG3C
void bindIndexBuffer(uint32_t ebo)
{
    if (s_vk.boundEbo != ebo) // PERF-RDG4R
    {
        s_vk.boundEbo = ebo;
        s_vk.dirtyIndexBuffer = true;
    }
    if (s_vk.boundVao != 0)
    {
        if (GpuVao *vao = s_vk.vaos.find(s_vk.boundVao)) // PERF-RDG4R
        {
            if (vao->ebo != ebo)
            {
                vao->ebo = ebo;
                s_vk.dirtyIndexBuffer = true;
            }
        }
    }
}

void setVertexAttribute(uint32_t index, int, rdgfx::VertexAttrFormat, bool, int, int offsetBytes)
{
    if (s_vk.boundVao != 0 && s_vk.boundVbo != 0)
    {
        if (GpuVao *vao = s_vk.vaos.find(s_vk.boundVao)) // PERF-RDG4R
        {
            vao->vbo = s_vk.boundVbo;
            if (index == 0)
                vao->vbOffset = static_cast<VkDeviceSize>(std::max(0, offsetBytes));
            s_vk.dirtyVertexBuffer = true;
        }
    }
}

void enableVertexAttribute(uint32_t)
{
    if (s_vk.boundVao != 0 && s_vk.boundVbo != 0)
    {
        if (GpuVao *vao = s_vk.vaos.find(s_vk.boundVao)) // PERF-RDG4R
        {
            if (vao->vbo != s_vk.boundVbo)
            {
                vao->vbo = s_vk.boundVbo;
                s_vk.dirtyVertexBuffer = true;
            }
        }
    }
}

    // PROBE-RDG3B
    auto logFirstDraw = [](const char *kind, uint32_t first, uint32_t count)
    {
        if (!s_vk.logEnabled || s_vk.firstDrawLogged)
            return;
        s_vk.firstDrawLogged = true;
        VkRect2D vkSc = computeVkScissor();
        const uint32_t activeVbo = resolveBoundVbo(); // FIX-RDG3C
        const uint32_t activeEbo = resolveBoundEbo(); // FIX-RDG3C
        uint64_t vbHandle = 0;
        int vbSize = 0;
        if (const GpuBuffer *vbo = s_vk.vbos.find(activeVbo); vbo && s_vk.curFrame < vbo->copies.size())
        {
            vbHandle = reinterpret_cast<uint64_t>(vbo->copies[s_vk.curFrame].buffer);
            vbSize = vbo->sizeBytes;
        }
        uint64_t ibHandle = 0;
        int ibSize = 0;
        if (const GpuBuffer *ebo = s_vk.ebos.find(activeEbo); ebo && s_vk.curFrame < ebo->copies.size())
        {
            ibHandle = reinterpret_cast<uint64_t>(ebo->copies[s_vk.curFrame].buffer);
            ibSize = ebo->sizeBytes;
        }
        std::printf("[rdvk] firstDraw=%s target=%ux%u vp=(%d,%d,%d,%d) scOn=%d sc=(%d,%d,%d,%d) vkSc=(%d,%d,%u,%u)"
                    " vbId=%u vb=0x%llx vbSize=%d ibId=%u ib=0x%llx ibSize=%d first=%u count=%u\n",
                    kind,
                    s_vk.targetW, s_vk.targetH,
                    s_vk.vpX, s_vk.vpY, s_vk.vpW, s_vk.vpH,
                    s_vk.scissorTest ? 1 : 0,
                    s_vk.scX, s_vk.scY, s_vk.scW, s_vk.scH,
                    vkSc.offset.x, vkSc.offset.y, vkSc.extent.width, vkSc.extent.height,
                    activeVbo, static_cast<unsigned long long>(vbHandle), vbSize,
                    activeEbo, static_cast<unsigned long long>(ibHandle), ibSize,
                    first, count);
    };

void drawArrays(rdgfx::PrimitiveTopology, int first, int count)
{
    ProbeDrawScope drawProbe; // PROBE-RDG4P
    ++s_vk.statDrawArr; // PROBE-RDG3B
    logFirstDraw("drawArrays", static_cast<uint32_t>(first), static_cast<uint32_t>(std::max(0, count)));
    if (count <= 0)
    {
        ++s_vk.statZeroCount; // PROBE-RDG3B
        return;
    }
    if (!prepareDraw())
        return;
    const bool fg5On = fg5ProfActive(); // PROBE-FG5 // FEAT-FG6
    const int64_t fg5DrawT0 = fg5On ? probeQpcNow() : 0; // PROBE-FG5
    const uint32_t firstInst = fg6StateCacheEnabled() ? (s_vk.curAtInvert != 0 ? 1u : 0u) : 0u; // FIX-FG8
    vkCmdDraw(s_vk.cmd, static_cast<uint32_t>(count), 1, static_cast<uint32_t>(first), firstInst); // FIX-FG8
    if (s_vk.enhTrack) // FEAT-ENH2
        enhTrackDraw(count);
    if (fg5On) // PROBE-FG5
        ps2xRdDrawProfAddVk(0, 0, probeQpcNow() - fg5DrawT0, 0, 0, 1);
}

void drawIndexed(rdgfx::PrimitiveTopology, uint32_t first, uint32_t count, bool idx32)
{
    ProbeDrawScope drawProbe; // PROBE-RDG4P
    ++s_vk.statDrawIdx; // PROBE-RDG3B
    logFirstDraw("drawIndexed", first, count);
    if (count == 0)
    {
        ++s_vk.statZeroCount; // PROBE-RDG3B
        return;
    }
    if (!prepareDraw())
        return;
    const bool fg5On = fg5ProfActive(); // PROBE-FG5 // FEAT-FG6
    const int64_t fg5IbT0 = fg5On ? probeQpcNow() : 0; // PROBE-FG5
    uint64_t fg5IbChg = 0; // PROBE-FG5
    // PERF-RDG4R: only resolve and bind index buffer when dirty or index type changed.
    const VkIndexType idxType = idx32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (s_vk.dirtyIndexBuffer || s_vk.lastBoundIb == VK_NULL_HANDLE || idxType != s_vk.lastBoundIbType)
    {
        const uint32_t activeEbo = resolveBoundEbo(); // FIX-RDG3C
        const GpuBuffer *ebo = s_vk.ebos.find(activeEbo);
        if (!ebo)
        {
            ++s_vk.statNoIb; // PROBE-RDG3B
            return;
        }
        VkBuffer eb = ebo->copies[s_vk.curFrame].buffer;
        if (eb != s_vk.lastBoundIb || idxType != s_vk.lastBoundIbType)
        {
            vkCmdBindIndexBuffer(s_vk.cmd, eb, 0, idxType);
            s_vk.lastBoundIb = eb;
            s_vk.lastBoundIbType = idxType;
            if (fg5On) fg5IbChg = 1; // PROBE-FG5
        }
        s_vk.dirtyIndexBuffer = false;
    }
    const int64_t fg5DrawT0 = fg5On ? probeQpcNow() : 0; // PROBE-FG5
    const uint32_t firstInst = fg6StateCacheEnabled() ? (s_vk.curAtInvert != 0 ? 1u : 0u) : 0u; // FIX-FG8
    vkCmdDrawIndexed(s_vk.cmd, count, 1, first, 0, firstInst); // FIX-FG8
    if (s_vk.enhTrack) // FEAT-ENH2
        enhTrackDraw(count);
    if (fg5On) // PROBE-FG5 / PROBE-FG9
    {
        const int64_t fg5T1 = probeQpcNow();
        ps2xRdDrawProfAddVk(fg5DrawT0 - fg5IbT0, 0, fg5T1 - fg5DrawT0, fg5IbChg, 0, 1);
        if (fg5DrawT0 > fg5IbT0 || fg5IbChg != 0)
            ps2xRdDrawProfAddFg9Bind(0, 0, 0, 0, 0, 0, 0, fg5DrawT0 - fg5IbT0, fg5IbChg, 0, 0, 0, 0);
    }
}

uint32_t createTexture2DInternal(const void *data, int w, int h, uint32_t mipLevels, const SamplerKey &sampler, bool clearBlackOpaque) // RDG2B
{
    ProbeTexScope texProbe; // PROBE-RDG4P
    if (!s_vk.initialized || w <= 0 || h <= 0 || mipLevels == 0)
        return 0;

    GpuTexture tex;
    tex.width = w;
    tex.height = h;
    tex.mipLevels = mipLevels;
    tex.sampler = sampler;

    // FIX-V3E: check pool before creating image
    const uint64_t poolKey = imagePoolKey(static_cast<uint32_t>(w), static_cast<uint32_t>(h), mipLevels);
    auto poolIt = s_vk.imagePool.find(poolKey);
    if (poolIt != s_vk.imagePool.end())
    {
        tex.image = poolIt->second.image;
        tex.memory = poolIt->second.memory;
        tex.view = poolIt->second.view;
        tex.memBytes = poolIt->second.bytes;
        if (s_vk.poolTotalBytes >= poolIt->second.bytes)
            s_vk.poolTotalBytes -= poolIt->second.bytes;
        else
            s_vk.poolTotalBytes = 0;
        s_vk.imagePool.erase(poolIt);
        ++s_vk.statTexPoolHit;
    }
    else
    {
        ++s_vk.statTexPoolMiss;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        ici.mipLevels = mipLevels;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(s_vk.device, &ici, nullptr, &tex.image) != VK_SUCCESS)
            return 0;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(s_vk.device, tex.image, &req);
        uint32_t memType = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX)
        {
            vkDestroyImage(s_vk.device, tex.image, nullptr);
            return 0;
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType;
        if (vkAllocateMemory(s_vk.device, &mai, nullptr, &tex.memory) != VK_SUCCESS ||
            vkBindImageMemory(s_vk.device, tex.image, tex.memory, 0) != VK_SUCCESS)
        {
            if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(s_vk.device, tex.memory, nullptr);
            vkDestroyImage(s_vk.device, tex.image, nullptr);
            return 0;
        }

        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = tex.image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
        if (vkCreateImageView(s_vk.device, &ivci, nullptr, &tex.view) != VK_SUCCESS)
        {
            vkFreeMemory(s_vk.device, tex.memory, nullptr);
            vkDestroyImage(s_vk.device, tex.image, nullptr);
            return 0;
        }
        tex.memBytes = req.size;
    }

    // FIX-V3D: Staging ring with fallback
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(w) * h * 4;
    VkBuffer uploadBuf = VK_NULL_HANDLE;
    VkDeviceSize uploadOffset = 0;
    BufferCopy fallbackStaging{};
    bool usedRing = false;

    if (data)
    {
        if (s_vk.frameActive)
        {
            const VkDeviceSize ringSize = 64ULL * 1024 * 1024;
            BufferCopy &ring = s_vk.stagingRings[s_vk.curFrame];
            if (ring.buffer == VK_NULL_HANDLE)
                createHostBuffer(ringSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, ring);

            const VkDeviceSize align = 16;
            const VkDeviceSize aligned = (s_vk.stagingCursors[s_vk.curFrame] + (align - 1)) & ~(align - 1);
            if (ring.buffer != VK_NULL_HANDLE && ring.mapped != nullptr && aligned + byteSize <= ringSize)
            {
                usedRing = true;
                uploadBuf = ring.buffer;
                uploadOffset = aligned;
                std::memcpy(static_cast<uint8_t *>(ring.mapped) + aligned, data, static_cast<size_t>(byteSize));
                s_vk.stagingCursors[s_vk.curFrame] = aligned + byteSize;
            }
        }

        if (!usedRing)
        {
            if (!createHostBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, fallbackStaging))
            {
                vkDestroyImageView(s_vk.device, tex.view, nullptr);
                vkFreeMemory(s_vk.device, tex.memory, nullptr);
                vkDestroyImage(s_vk.device, tex.image, nullptr);
                return 0;
            }
            uploadBuf = fallbackStaging.buffer;
            uploadOffset = 0;
            std::memcpy(fallbackStaging.mapped, data, static_cast<size_t>(byteSize));
        }
    }

    VkCommandBuffer cmd = beginUploadCmdBatched(); // FIX-V3G
    if (data)
    {
        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.bufferOffset = uploadOffset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyBufferToImage(cmd, uploadBuf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    else if (clearBlackOpaque)
    {
        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue cv{{0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
        vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);

        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    else
    {
        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    endUploadCmdBatched(cmd); // FIX-V3G

    if (data && !usedRing)
    {
        vkUnmapMemory(s_vk.device, fallbackStaging.memory);
        if (s_vk.frameActive)
        {
            s_vk.garbageBuffers[s_vk.curFrame].push_back({fallbackStaging.buffer, fallbackStaging.memory}); // FIX-V3D
        }
        else
        {
            vkDestroyBuffer(s_vk.device, fallbackStaging.buffer, nullptr);
            vkFreeMemory(s_vk.device, fallbackStaging.memory, nullptr);
        }
    }

    const uint32_t id = s_vk.textures.allocate(std::move(tex)); // PERF-RDG4R
    // Vector reallocation can move GpuTexture slots, so invalidate cached texture pointers.
    s_vk.cachedTex[0] = nullptr;
    s_vk.cachedTex[1] = nullptr;
    s_vk.dirtyTextures = true;
    ++s_vk.statTexCreated; // PROBE-RDG3B
    return id;
}

uint32_t createTextureRGBA8(const void *data, int w, int h)
{
    ProbeTexScope texProbe; // PROBE-RDG4P
    if (!s_vk.initialized || w <= 0 || h <= 0)
        return 0;

    const uint32_t maxDim = static_cast<uint32_t>(std::max(w, h));
    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) + 1u;
    return createTexture2DInternal(data, w, h, mipLevels, SamplerKey{}, false);
}

void freeTexture(uint32_t texId)
{
    const GpuTexture *tex = s_vk.textures.find(texId); // PERF-RDG4R
    if (!tex)
        return;
    if (s_vk.frameActive)
    {
        // FIX-V3E: preserve w, h, mips, bytes for recycling in imagePool
        const uint64_t bytes = tex->memBytes > 0 ? tex->memBytes : (static_cast<uint64_t>(std::max(0, tex->width)) * std::max(0, tex->height) * 4);
        s_vk.garbageTextures[s_vk.curFrame].push_back({tex->view, tex->image, tex->memory,
                                                       static_cast<uint32_t>(std::max(0, tex->width)),
                                                       static_cast<uint32_t>(std::max(0, tex->height)),
                                                       tex->mipLevels,
                                                       bytes});
    }
    else
    {
        if (tex->view != VK_NULL_HANDLE) vkDestroyImageView(s_vk.device, tex->view, nullptr);
        if (tex->image != VK_NULL_HANDLE) vkDestroyImage(s_vk.device, tex->image, nullptr);
        if (tex->memory != VK_NULL_HANDLE) vkFreeMemory(s_vk.device, tex->memory, nullptr);
    }
    s_vk.textures.erase(texId);
    if (texId == s_vk.boundTex[0] || texId == s_vk.boundTex[1] ||
        texId == s_vk.cachedEffTexId[0] || texId == s_vk.cachedEffTexId[1])
    {
        s_vk.cachedTex[0] = nullptr;
        s_vk.cachedTex[1] = nullptr;
        s_vk.dirtyTextures = true;
    }
}

void generateMipmaps(uint32_t texId, int w, int h)
{
    ProbeTexScope texProbe; // PROBE-RDG4P
    GpuTexture *texPtr = s_vk.textures.find(texId); // PERF-RDG4R
    if (!texPtr)
        return;
    GpuTexture &tex = *texPtr;
    if (tex.mipLevels <= 1)
        return;

    VkCommandBuffer cmd = beginUploadCmdBatched(); // FIX-V3G
    imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, tex.mipLevels,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    int32_t mipW = w > 0 ? w : tex.width;
    int32_t mipH = h > 0 ? h : tex.height;
    for (uint32_t i = 1; i < tex.mipLevels; ++i)
    {
        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        int32_t nextW = std::max(1, mipW / 2);
        int32_t nextH = std::max(1, mipH / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1] = {mipW, mipH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1] = {nextW, nextH, 1};
        vkCmdBlitImage(cmd,
                       tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        mipW = nextW;
        mipH = nextH;
    }

    imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, tex.mipLevels - 1, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    endUploadCmdBatched(cmd); // FIX-V3G
}

void setTextureWrap(uint32_t texId, bool isT, rdgfx::TextureWrap wrap)
{
    GpuTexture *tex = s_vk.textures.find(texId); // PERF-RDG4R
    if (!tex)
        return;
    bool changed = false;
    if (isT)
    {
        if (tex->sampler.wrapT != wrap)
        {
            tex->sampler.wrapT = wrap;
            changed = true;
        }
    }
    else
    {
        if (tex->sampler.wrapS != wrap)
        {
            tex->sampler.wrapS = wrap;
            changed = true;
        }
    }
    if (changed && (texId == s_vk.boundTex[0] || texId == s_vk.boundTex[1] ||
                    texId == s_vk.cachedEffTexId[0] || texId == s_vk.cachedEffTexId[1]))
    {
        s_vk.dirtyTextures = true;
    }
}

void setTextureFilter(uint32_t texId, bool isMin, rdgfx::TextureFilter filter)
{
    GpuTexture *tex = s_vk.textures.find(texId); // PERF-RDG4R
    if (!tex)
        return;
    bool changed = false;
    if (isMin)
    {
        if (tex->sampler.minFilter != filter)
        {
            tex->sampler.minFilter = filter;
            changed = true;
        }
    }
    else
    {
        if (tex->sampler.magFilter != filter)
        {
            tex->sampler.magFilter = filter;
            changed = true;
        }
    }
    if (changed && (texId == s_vk.boundTex[0] || texId == s_vk.boundTex[1] ||
                    texId == s_vk.cachedEffTexId[0] || texId == s_vk.cachedEffTexId[1]))
    {
        s_vk.dirtyTextures = true;
    }
}

void bindTexture(uint32_t unit, uint32_t texId)
{
    ++s_vk.statBinds; // PROBE-RDG3B
    if (unit < 2 && s_vk.boundTex[unit] != texId) // PERF-RDG4R
    {
        s_vk.boundTex[unit] = texId;
        s_vk.dirtyTextures = true;
    }
}

// RDG2B: palette textures, overlay quad, backbuffer copy
bool isPaletteTextureSupported() { return s_vk.initialized; }

bool uploadPaletteTexture(const uint32_t *rgba256)
{
    ProbeTexScope texProbe; // PROBE-RDG4P
    if (!s_vk.initialized || !rgba256)
        return false;

    const uint32_t fi = (s_vk.curFrame < s_vk.framesInFlight) ? s_vk.curFrame : 0u;
    uint32_t &palId = s_vk.palTexIds[fi];
    constexpr size_t kPalBytes = 256u * sizeof(uint32_t);

    if (palId == 0u)
    {
        SamplerKey palSamp{rdgfx::TextureFilter::Nearest, rdgfx::TextureFilter::Nearest,
                           rdgfx::TextureWrap::Clamp, rdgfx::TextureWrap::Clamp};
        palId = createTexture2DInternal(rgba256, 256, 1, 1, palSamp, false);
        if (palId == 0u)
            return false;
        std::memcpy(s_vk.palLastRgba[fi].data(), rgba256, kPalBytes);
        s_vk.palHasData[fi] = true;
    }
    else if (!s_vk.palHasData[fi] || std::memcmp(s_vk.palLastRgba[fi].data(), rgba256, kPalBytes) != 0)
    {
        const GpuTexture *tex = s_vk.textures.find(palId); // PERF-RDG4R
        if (!tex)
            return false;

        BufferCopy staging{};
        if (!createHostBuffer(static_cast<VkDeviceSize>(kPalBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging))
            return false;
        std::memcpy(staging.mapped, rgba256, kPalBytes);

        if (s_vk.frameActive && s_vk.cmd != VK_NULL_HANDLE)
        {
            if (s_vk.inRenderPass)
            {
                vkCmdEndRendering(s_vk.cmd);
                s_vk.inRenderPass = false;
                invalidateCommandBufferState(); // PERF-RDG4R
            }
            imageBarrier(s_vk.cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {256u, 1u, 1u};
            vkCmdCopyBufferToImage(s_vk.cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            imageBarrier(s_vk.cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            vkUnmapMemory(s_vk.device, staging.memory);
            s_vk.garbageBuffers[s_vk.curFrame].push_back({staging.buffer, staging.memory});
        }
        else
        {
            VkCommandBuffer cmd = beginOneTimeCmd();
            imageBarrier(cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {256u, 1u, 1u};
            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            imageBarrier(cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            endOneTimeCmd(cmd);

            vkUnmapMemory(s_vk.device, staging.memory);
            vkDestroyBuffer(s_vk.device, staging.buffer, nullptr);
            vkFreeMemory(s_vk.device, staging.memory, nullptr);
        }

        std::memcpy(s_vk.palLastRgba[fi].data(), rgba256, kPalBytes);
        s_vk.palHasData[fi] = true;
    }

    if (s_vk.boundTex[1] != palId) // PERF-RDG4R
    {
        s_vk.boundTex[1] = palId;
        s_vk.dirtyTextures = true;
    }
    return true;
}

void unbindPaletteTexture()
{
    if (s_vk.boundTex[1] != 0u) // PERF-RDG4R
    {
        s_vk.boundTex[1] = 0u;
        s_vk.dirtyTextures = true;
    }
}

void drawTextureOverlay(uint32_t texId, int texW, int texH,
                        float srcX, float srcY, float srcW, float srcH,
                        float dstX, float dstY, float dstW, float dstH,
                        uint8_t alpha)
{
    if (!s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE ||
        texId == 0u || texW <= 0 || texH <= 0 || dstW == 0.0f || dstH == 0.0f)
        return;
    if (!s_vk.textures.find(texId)) // PERF-RDG4R
        return;

    struct OverlayVertex
    {
        float pos[4];
        uint8_t col[4];
        float uv[2];
        float fog; // FIX-E1b
    };
    static_assert(sizeof(OverlayVertex) == 32, "OverlayVertex must match 32-byte stride"); // FIX-E1b
    constexpr uint32_t kMaxOverlayQuads = 64u;
    constexpr uint32_t kMaxOverlayVerts = kMaxOverlayQuads * 6u;

    if (s_vk.overlayVbo == 0u)
    {
        s_vk.overlayVbo = createDynBuffer(nullptr, static_cast<int>(kMaxOverlayVerts * sizeof(OverlayVertex)), false);
        if (s_vk.overlayVbo == 0u)
            return;
    }
    if (s_vk.overlayVertexCursor + 6u > kMaxOverlayVerts)
        return;

    const GpuBuffer *ovVbo = s_vk.vbos.find(s_vk.overlayVbo); // PERF-RDG4R
    if (!ovVbo || s_vk.curFrame >= ovVbo->copies.size() || !ovVbo->copies[s_vk.curFrame].mapped)
        return;

    const float u0 = srcX / static_cast<float>(texW);
    const float v0 = srcY / static_cast<float>(texH);
    const float u1 = (srcX + srcW) / static_cast<float>(texW);
    const float v1 = (srcY + srcH) / static_cast<float>(texH);
    const float x0 = dstX;
    const float y0 = dstY;
    const float x1 = dstX + dstW;
    const float y1 = dstY + dstH;

    const OverlayVertex quad[6] = {
        {{x0, y0, 0.0f, 1.0f}, {255, 255, 255, 255}, {u0, v0}, 1.0f}, // FIX-E1b
        {{x1, y0, 0.0f, 1.0f}, {255, 255, 255, 255}, {u1, v0}, 1.0f}, // FIX-E1b
        {{x1, y1, 0.0f, 1.0f}, {255, 255, 255, 255}, {u1, v1}, 1.0f}, // FIX-E1b
        {{x0, y0, 0.0f, 1.0f}, {255, 255, 255, 255}, {u0, v0}, 1.0f}, // FIX-E1b
        {{x1, y1, 0.0f, 1.0f}, {255, 255, 255, 255}, {u1, v1}, 1.0f}, // FIX-E1b
        {{x0, y1, 0.0f, 1.0f}, {255, 255, 255, 255}, {u0, v1}, 1.0f}, // FIX-E1b
    };

    const uint32_t firstVertex = s_vk.overlayVertexCursor;
    uint8_t *dstPtr = static_cast<uint8_t *>(ovVbo->copies[s_vk.curFrame].mapped) + firstVertex * sizeof(OverlayVertex);
    std::memcpy(dstPtr, quad, sizeof(quad));

    // Save state
    const PushConstants savedPc = s_vk.pc;
    const PipelineKey savedPipeKey = s_vk.pipeKey;
    const int savedVpX = s_vk.vpX, savedVpY = s_vk.vpY, savedVpW = s_vk.vpW, savedVpH = s_vk.vpH;
    const bool savedScissor = s_vk.scissorTest;
    const uint32_t savedVao = s_vk.boundVao;
    const uint32_t savedVbo = s_vk.boundVbo;
    const uint32_t savedTex0 = s_vk.boundTex[0];
    const uint32_t savedTex1 = s_vk.boundTex[1];

    // Set 2D overlay state
    s_vk.pc = PushConstants{};
    s_vk.pc.uFlip[0] = 1.0f;
    s_vk.pc.uFlip[1] = 1.0f;
    s_vk.pc.uFlip[2] = 1.0f;
    s_vk.pc.uMode2d = 1;
    s_vk.pc.uFbSize[0] = static_cast<float>(s_vk.targetW);
    s_vk.pc.uFbSize[1] = static_cast<float>(s_vk.targetH);
    s_vk.pc.uTex = 1;
    s_vk.pc.uTfx = 1;
    s_vk.pc.uTcc = 0;
    s_vk.pc.uAtst = 1;
    s_vk.pc.uFixAlpha = static_cast<float>(alpha) / 255.0f;

    s_vk.pipeKey.blendEnable = true;
    s_vk.pipeKey.srcRgb = rdgfx::BlendFactor::SrcAlpha;
    s_vk.pipeKey.dstRgb = rdgfx::BlendFactor::OneMinusSrcAlpha;
    s_vk.pipeKey.srcAlpha = rdgfx::BlendFactor::SrcAlpha;
    s_vk.pipeKey.dstAlpha = rdgfx::BlendFactor::OneMinusSrcAlpha;
    s_vk.pipeKey.eqRgb = rdgfx::BlendEq::Add;
    s_vk.pipeKey.eqAlpha = rdgfx::BlendEq::Add;
    s_vk.pipeKey.colorMask = 0x0Fu;
    s_vk.pipeKey.depthTest = false;
    s_vk.pipeKey.depthMask = false;
    s_vk.pipeKey.cull = false;

    s_vk.vpX = 0;
    s_vk.vpY = 0;
    s_vk.vpW = static_cast<int>(s_vk.targetW);
    s_vk.vpH = static_cast<int>(s_vk.targetH);
    s_vk.scissorTest = false;
    s_vk.boundVao = 0u;
    s_vk.boundVbo = s_vk.overlayVbo;
    s_vk.boundTex[0] = texId;
    s_vk.boundTex[1] = 0u;
    // PERF-RDG4R: mark dirty before overlay draw
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
    s_vk.dirtyPipeline = true;
    s_vk.dirtyViewport = true;
    s_vk.dirtyScissor = true;
    s_vk.dirtyVertexBuffer = true;
    s_vk.dirtyTextures = true;

    if (prepareDraw())
    {
        vkCmdDraw(s_vk.cmd, 6u, 1u, firstVertex, 0u);
        s_vk.overlayVertexCursor += 6u;
    }

    // Restore state
    s_vk.pc = savedPc;
    s_vk.pipeKey = savedPipeKey;
    s_vk.vpX = savedVpX;
    s_vk.vpY = savedVpY;
    s_vk.vpW = savedVpW;
    s_vk.vpH = savedVpH;
    s_vk.scissorTest = savedScissor;
    s_vk.boundVao = savedVao;
    s_vk.boundVbo = savedVbo;
    s_vk.boundTex[0] = savedTex0;
    s_vk.boundTex[1] = savedTex1;
    // PERF-RDG4R: mark dirty after restoring state
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
    s_vk.dirtyPipeline = true;
    s_vk.dirtyViewport = true;
    s_vk.dirtyScissor = true;
    s_vk.dirtyVertexBuffer = true;
    s_vk.dirtyTextures = true;
}

static void drawQuad2d(uint32_t texId,
                       float u0, float v0, float u1, float v1,
                       float dstX, float dstY, float dstW, float dstH,
                       const PushConstants &pc,
                       bool blendEnable,
                       rdgfx::BlendFactor srcRgb, rdgfx::BlendFactor dstRgb,
                       rdgfx::BlendFactor srcAlpha, rdgfx::BlendFactor dstAlpha) // FXC1
{
    if (!s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE ||
        texId == 0u || dstW == 0.0f || dstH == 0.0f || !s_vk.textures.find(texId))
        return;

    struct OverlayVertex
    {
        float pos[4];
        uint8_t col[4];
        float uv[2];
        float fog; // FIX-PA2
    };
    static_assert(sizeof(OverlayVertex) == 32, "OverlayVertex must match 32-byte stride"); // FIX-PA2
    constexpr uint32_t kMaxOverlayVerts = 64u * 6u;
    if (s_vk.overlayVbo == 0u)
    {
        s_vk.overlayVbo = createDynBuffer(nullptr, static_cast<int>(kMaxOverlayVerts * sizeof(OverlayVertex)), false);
        if (s_vk.overlayVbo == 0u)
            return;
    }
    if (s_vk.overlayVertexCursor + 6u > kMaxOverlayVerts)
        return;

    const GpuBuffer *ovVbo = s_vk.vbos.find(s_vk.overlayVbo);
    if (!ovVbo || s_vk.curFrame >= ovVbo->copies.size() || !ovVbo->copies[s_vk.curFrame].mapped)
        return;

    const float x0 = dstX;
    const float y0 = dstY;
    const float x1 = dstX + dstW;
    const float y1 = dstY + dstH;
    const OverlayVertex quad[6] = {
        {{x0, y0, 0.0f, 1.0f}, {255, 255, 255, 255}, {u0, v0}, 1.0f}, // FIX-PA2
        {{x1, y0, 0.0f, 1.0f}, {255, 255, 255, 255}, {u1, v0}, 1.0f}, // FIX-PA2
        {{x1, y1, 0.0f, 1.0f}, {255, 255, 255, 255}, {u1, v1}, 1.0f}, // FIX-PA2
        {{x0, y0, 0.0f, 1.0f}, {255, 255, 255, 255}, {u0, v0}, 1.0f}, // FIX-PA2
        {{x1, y1, 0.0f, 1.0f}, {255, 255, 255, 255}, {u1, v1}, 1.0f}, // FIX-PA2
        {{x0, y1, 0.0f, 1.0f}, {255, 255, 255, 255}, {u0, v1}, 1.0f}, // FIX-PA2
    };

    const uint32_t firstVertex = s_vk.overlayVertexCursor;
    uint8_t *dstPtr = static_cast<uint8_t *>(ovVbo->copies[s_vk.curFrame].mapped) + firstVertex * sizeof(OverlayVertex);
    std::memcpy(dstPtr, quad, sizeof(quad));

    const PushConstants savedPc = s_vk.pc;
    const PipelineKey savedPipeKey = s_vk.pipeKey;
    const int savedVpX = s_vk.vpX, savedVpY = s_vk.vpY, savedVpW = s_vk.vpW, savedVpH = s_vk.vpH;
    const bool savedScissor = s_vk.scissorTest;
    const uint32_t savedVao = s_vk.boundVao;
    const uint32_t savedVbo = s_vk.boundVbo;
    const uint32_t savedTex0 = s_vk.boundTex[0];
    const uint32_t savedTex1 = s_vk.boundTex[1];

    s_vk.pc = pc;
    s_vk.pipeKey.blendEnable = blendEnable;
    s_vk.pipeKey.srcRgb = srcRgb;
    s_vk.pipeKey.dstRgb = dstRgb;
    s_vk.pipeKey.srcAlpha = srcAlpha;
    s_vk.pipeKey.dstAlpha = dstAlpha;
    s_vk.pipeKey.eqRgb = rdgfx::BlendEq::Add;
    s_vk.pipeKey.eqAlpha = rdgfx::BlendEq::Add;
    s_vk.pipeKey.colorMask = 0x0Fu;
    s_vk.pipeKey.depthTest = false;
    s_vk.pipeKey.depthMask = false;
    s_vk.pipeKey.cull = false;

    s_vk.vpX = 0;
    s_vk.vpY = 0;
    s_vk.vpW = static_cast<int>(s_vk.targetW);
    s_vk.vpH = static_cast<int>(s_vk.targetH);
    s_vk.scissorTest = false;
    s_vk.boundVao = 0u;
    s_vk.boundVbo = s_vk.overlayVbo;
    s_vk.boundTex[0] = texId;
    s_vk.boundTex[1] = 0u;
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
    s_vk.dirtyPipeline = true;
    s_vk.dirtyViewport = true;
    s_vk.dirtyScissor = true;
    s_vk.dirtyVertexBuffer = true;
    s_vk.dirtyTextures = true;

    if (prepareDraw())
    {
        vkCmdDraw(s_vk.cmd, 6u, 1u, firstVertex, 0u);
        s_vk.overlayVertexCursor += 6u;
    }

    s_vk.pc = savedPc;
    s_vk.pipeKey = savedPipeKey;
    s_vk.vpX = savedVpX;
    s_vk.vpY = savedVpY;
    s_vk.vpW = savedVpW;
    s_vk.vpH = savedVpH;
    s_vk.scissorTest = savedScissor;
    s_vk.boundVao = savedVao;
    s_vk.boundVbo = savedVbo;
    s_vk.boundTex[0] = savedTex0;
    s_vk.boundTex[1] = savedTex1;
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
    s_vk.dirtyPipeline = true;
    s_vk.dirtyViewport = true;
    s_vk.dirtyScissor = true;
    s_vk.dirtyVertexBuffer = true;
    s_vk.dirtyTextures = true;
}

static PushConstants makeCompositePc(float fbW, float fbH, int32_t palMode, int32_t palFallback) // FIX-FXC4
{
    static const struct FlipCfg {
        float x, y, z;
    } s_flip = []() {
        const char *fe = std::getenv("PS2X_RD_FLIP");
        const int flipBits = (fe && *fe) ? std::atoi(fe) : 0;
        return FlipCfg{
            1.0f,
            (flipBits & 1) ? 1.0f : -1.0f,
            (flipBits & 2) ? -1.0f : 1.0f,
        };
    }();
    PushConstants pc{};
    pc.uFlip[0] = s_flip.x;
    pc.uFlip[1] = s_flip.y;
    pc.uFlip[2] = s_flip.z;
    pc.uFlip[3] = 0.0f;
    pc.uMode2d = 1;
    pc.uFbSize[0] = fbW;
    pc.uFbSize[1] = fbH;
    pc.uZBias = 0.0f;
    pc.uVpB[3] = 0.0f;
    pc.uAtst = 1;
    pc.uAtInvert = 0;
    pc.uFixAlpha = -1.0f;
    pc.uTex = 1;
    pc.uTfx = 1;
    pc.uTcc = 0;
    pc.uPalMode = palMode;
    pc.uPalFallback = palFallback;
    return pc;
}

static void prepareCompositeState(int tw, int th,
                                  bool blendEnable,
                                  rdgfx::BlendFactor srcRgb, rdgfx::BlendFactor dstRgb,
                                  rdgfx::BlendFactor srcAlpha, rdgfx::BlendFactor dstAlpha) // FIX-FXC4
{
    rdgfx_vk::setViewport(0, 0, tw, th);
    rdgfx_vk::setScissorTest(false);
    rdgfx_vk::setScissorRect(0, 0, tw, th);
    rdgfx_vk::setColorMask(true, true, true, true);
    rdgfx_vk::setDepthTest(false);
    rdgfx_vk::setDepthMask(false);
    rdgfx_vk::setBackfaceCulling(false);
    rdgfx_vk::setBlend(blendEnable, srcRgb, dstRgb, srcAlpha, dstAlpha, rdgfx::BlendEq::Add, rdgfx::BlendEq::Add);
    rdgfx_vk::bindVertexArray(0u);
    s_vk.dirtyPipeline = true;
    s_vk.dirtyViewport = true;
    s_vk.dirtyScissor = true;
    s_vk.dirtyTextures = true;
    s_vk.dirtyPushConstants = true;
    s_vk.curAtInvert = 0; // FIX-FG8
    s_vk.dirtyVertexBuffer = true;
    s_vk.dirtyIndexBuffer = true;
}

void drawFxBackground(uint32_t texId, int texW, int texH) // FXC1
{
    if (!s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE ||
        texId == 0u || texW <= 0 || texH <= 0 || s_vk.targetW == 0u || s_vk.targetH == 0u)
        return;

    const int tw = static_cast<int>(s_vk.targetW);
    const int th = static_cast<int>(s_vk.targetH);
    prepareCompositeState(tw, th, false,
                          rdgfx::BlendFactor::One, rdgfx::BlendFactor::Zero,
                          rdgfx::BlendFactor::One, rdgfx::BlendFactor::Zero); // FIX-FXC4

    const PushConstants pc = makeCompositePc(static_cast<float>(tw), static_cast<float>(th), 1, 1); // FIX-FXC4

    drawQuad2d(texId,
               0.0f, 0.0f, 1.0f, 448.0f / 512.0f,
               0.0f, 0.0f, static_cast<float>(tw), static_cast<float>(th),
               pc,
               false,
               rdgfx::BlendFactor::One, rdgfx::BlendFactor::Zero,
               rdgfx::BlendFactor::One, rdgfx::BlendFactor::Zero);
}

void drawClassicAdditive(const uint8_t *rgba, int w, int h, int strideBytes) // FXC1
{
    if (!s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE ||
        !rgba || w <= 0 || h <= 0 || s_vk.targetW == 0u || s_vk.targetH == 0u)
        return;

    const int tw = static_cast<int>(s_vk.targetW);
    const int th = static_cast<int>(s_vk.targetH);
    prepareCompositeState(tw, th, true,
                          rdgfx::BlendFactor::SrcAlpha, rdgfx::BlendFactor::OneMinusSrcAlpha,
                          rdgfx::BlendFactor::SrcAlpha, rdgfx::BlendFactor::OneMinusSrcAlpha); // FIX-FXC5

    if (s_vk.classicTexId == 0u || s_vk.classicTexW != w || s_vk.classicTexH != h)
    {
        if (s_vk.classicTexId != 0u)
        {
            rdgfx_vk::freeTexture(s_vk.classicTexId);
            s_vk.classicTexId = 0u;
        }
        SamplerKey samp{rdgfx::TextureFilter::Linear, rdgfx::TextureFilter::Linear,
                        rdgfx::TextureWrap::Clamp, rdgfx::TextureWrap::Clamp};
        const auto t_ct0 = std::chrono::steady_clock::now(); // PROBE-V3B
        s_vk.classicTexId = createTexture2DInternal(nullptr, w, h, 1u, samp, false);
        const auto t_ct1 = std::chrono::steady_clock::now(); // PROBE-V3B
        const double ctMs = std::chrono::duration<double, std::milli>(t_ct1 - t_ct0).count(); // PROBE-V3B
        probe_v3b::cntCrTex++; // PROBE-V3B
        probe_v3b::msCrTex += ctMs; // PROBE-V3B
        probe_v3b::allocs++; // PROBE-V3B
        if (s_vk.classicTexId == 0u)
            return;
        s_vk.classicTexW = w;
        s_vk.classicTexH = h;
    }

    const GpuTexture *tex = s_vk.textures.find(s_vk.classicTexId);
    if (!tex)
        return;

    const uint32_t rowBytes = static_cast<uint32_t>(w) * 4u;
    const uint32_t srcStride = (strideBytes > 0) ? static_cast<uint32_t>(strideBytes) : rowBytes;
    const VkDeviceSize totalBytes = static_cast<VkDeviceSize>(rowBytes) * static_cast<uint32_t>(h);

    const auto t_c0 = std::chrono::steady_clock::now(); // PROBE-V3B
    BufferCopy staging{};
    if (!createHostBuffer(totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging))
        return;
    const auto t_c1 = std::chrono::steady_clock::now(); // PROBE-V3B

    const auto t_b0 = std::chrono::steady_clock::now(); // PROBE-V3B
    auto *dst = static_cast<uint8_t *>(staging.mapped); // FIX-V3C
    for (int y = 0; y < h; ++y)
    {
        const uint8_t *srcRow = rgba + static_cast<size_t>(y) * srcStride;
        auto *dstRow = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * rowBytes);
        for (int x = 0; x < w; ++x)
        {
            const uint8_t r = srcRow[x * 4 + 0];
            const uint8_t g = srcRow[x * 4 + 1];
            const uint8_t b = srcRow[x * 4 + 2];
            const uint8_t a = (std::max(r, std::max(g, b)) > 6u) ? 255u : 0u;
            dstRow[x] = static_cast<uint32_t>(r) |
                        (static_cast<uint32_t>(g) << 8) |
                        (static_cast<uint32_t>(b) << 16) |
                        (static_cast<uint32_t>(a) << 24);
        }
    }
    const auto t_b1 = std::chrono::steady_clock::now(); // PROBE-V3B
    const double cur_bMs = std::chrono::duration<double, std::milli>(t_b1 - t_b0).count(); // PROBE-V3B
    probe_v3b::bSum += cur_bMs; // PROBE-V3B
    if (cur_bMs > probe_v3b::bMax) probe_v3b::bMax = cur_bMs; // PROBE-V3B

    const auto t_c2 = std::chrono::steady_clock::now(); // PROBE-V3B
    if (s_vk.inRenderPass)
    {
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
        invalidateCommandBufferState();
    }

    imageBarrier(s_vk.cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
    vkCmdCopyBufferToImage(s_vk.cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    imageBarrier(s_vk.cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkUnmapMemory(s_vk.device, staging.memory);
    s_vk.garbageBuffers[s_vk.curFrame].push_back({staging.buffer, staging.memory});
    const auto t_c3 = std::chrono::steady_clock::now(); // PROBE-V3B
    const double cur_cMs = std::chrono::duration<double, std::milli>((t_c1 - t_c0) + (t_c3 - t_c2)).count(); // PROBE-V3B
    probe_v3b::cSum += cur_cMs; // PROBE-V3B
    if (cur_cMs > probe_v3b::cMax) probe_v3b::cMax = cur_cMs; // PROBE-V3B

    const float srcWidth = static_cast<float>(w);
    const float srcHeight = static_cast<float>(h);
    const float screenWidth = static_cast<float>(s_vk.targetW);
    const float screenHeight = static_cast<float>(s_vk.targetH);
    // FIX-P2: read aspect mode every frame from ps2xGfxGetAspectMode()
    const int aspectMode = ::ps2xGfxGetAspectMode();
    float aspect = 4.0f / 3.0f;
    if (aspectMode == 2)
        aspect = 16.0f / 9.0f;
    else if (aspectMode == 3)
        aspect = 1.6f;
    else if (aspectMode == 4)
        aspect = 0.0f; // native/source
    else
        aspect = 4.0f / 3.0f; // 0 and 1(fill) => 4/3 for this CPU frame

    float dstWidth, dstHeight; // FIX-P2
    if (aspect > 0.0f) // FIX-P2
    {
        dstWidth = std::min(screenWidth, screenHeight * aspect);
        dstHeight = dstWidth / aspect;
    }
    else // FIX-P2
    {
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        dstWidth = srcWidth * scale;
        dstHeight = srcHeight * scale;
    }
    const float dstX = (screenWidth - dstWidth) * 0.5f;
    const float dstY = (screenHeight - dstHeight) * 0.5f;

    PushConstants pc = makeCompositePc(screenWidth, screenHeight, 0, 0); // FIX-FXC5
    pc.uTcc = 1; // FIX-FXC5

    drawQuad2d(s_vk.classicTexId,
               0.0f, 0.0f, 1.0f, 1.0f,
               dstX, dstY, dstWidth, dstHeight,
               pc,
               true,
               rdgfx::BlendFactor::SrcAlpha, rdgfx::BlendFactor::OneMinusSrcAlpha,
               rdgfx::BlendFactor::SrcAlpha, rdgfx::BlendFactor::OneMinusSrcAlpha); // FIX-FXC5
}

bool canCopyBackbuffer() { return s_vk.initialized; }

bool beginBackbufferCopy(rdgfx::BackbufferCopyState &state)
{
    state = rdgfx::BackbufferCopyState{};
    if (!s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE)
        return false;
    state.valid = true;
    return true;
}

bool copyBackbufferToTexture(rdgfx::BackbufferCopyState &state,
                             uint32_t &texId, uint32_t &auxId,
                             int &texW, int &texH,
                             int wantW, int wantH,
                             int vx, int vy, int vw, int vh,
                             int dstW, int dstH,
                             bool linearFilter)
{
    ProbeTexScope texProbe; // PROBE-RDG4P
    (void)auxId;
    if (!state.valid || !s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE)
        return false;
    if (wantW <= 0 || wantH <= 0 || vw <= 0 || vh <= 0 || dstW <= 0 || dstH <= 0)
        return false;

    if (texId == 0u || texW != wantW || texH != wantH)
    {
        if (texId != 0u)
        {
            rdgfx_vk::freeTexture(texId);
            texId = 0u;
        }
        SamplerKey copySamp{rdgfx::TextureFilter::Linear, rdgfx::TextureFilter::Linear,
                            rdgfx::TextureWrap::Clamp, rdgfx::TextureWrap::Clamp};
        texId = createTexture2DInternal(nullptr, wantW, wantH, 1u, copySamp, true);
        if (texId == 0u)
            return false;
        texW = wantW;
        texH = wantH;
    }

    const GpuTexture *tex = s_vk.textures.find(texId); // PERF-RDG4R
    if (!tex)
        return false;

    const int tw = static_cast<int>(s_vk.targetW);
    const int th = static_cast<int>(s_vk.targetH);
    const int sx0 = std::max(0, std::min(tw, vx));
    const int sx1 = std::max(0, std::min(tw, vx + vw));
    const int sy0 = std::max(0, std::min(th, th - (vy + vh)));
    const int sy1 = std::max(0, std::min(th, th - vy));
    if (sx1 <= sx0 || sy1 <= sy0)
        return false;

    if (s_vk.inRenderPass)
    {
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
        invalidateCommandBufferState(); // PERF-RDG4R
    }
    else if (s_vk.firstPassInFrame)
    {
        ensureRenderPass(true, 0.0f, 0.0f, 0.0f, 1.0f);
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
        invalidateCommandBufferState(); // PERF-RDG4R
    }

    const bool useMsaa = (s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT && s_vk.resolveImage != VK_NULL_HANDLE);
    VkImage srcImage = useMsaa ? s_vk.resolveImage : s_vk.colorImage;
    VkImageLayout &srcLayout = useMsaa ? s_vk.resolveLayout : s_vk.colorLayout;

    imageBarrier(s_vk.cmd, srcImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    imageBarrier(s_vk.cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {sx0, sy0, 0};
    blit.srcOffsets[1] = {sx1, sy1, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {std::min(dstW, texW), std::min(dstH, texH), 1};
    vkCmdBlitImage(s_vk.cmd,
                   srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    imageBarrier(s_vk.cmd, srcImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    srcLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    imageBarrier(s_vk.cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return true;
}

void endBackbufferCopy(const rdgfx::BackbufferCopyState &) {}

uint32_t getLastError() { return 0u; }

#include "rd_enhance_vk.inc" // FEAT-ENH1

} // namespace rdgfx_vk

// FEAT-ENH1
void ps2xEnhanceApply(const RdEnhanceFrame &frame)
{
    if (rdgfx_vk::active())
        rdgfx_vk::enhanceApply(frame);
}

#endif // PS2X_HAS_SDL3
