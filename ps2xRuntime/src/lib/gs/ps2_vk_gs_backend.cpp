// ps2_vk_gs_backend.cpp -- Vulkan GS backend, stages V1+V2 (docs/VULKAN_PLAN.md).
//
// GSVulkanBackend wraps a GSCpuBackend. Triangle and sprite primitives with supported formats are drawn on the GPU by a
// fragment shader (vk/gs_draw.frag) that reads/writes a GPU copy of the 4 MB GS VRAM (storage buffer) with the exact PS2 swizzle.
// Everything else (transfers, CLUT loads, lines/points, presentation, unsupported formats) runs on the wrapped CPU backend,
// with the two VRAM copies synchronised by conservative byte ranges:
//   gpuColor_/gpuZ_ = 8 KB pages the GPU wrote that the host copy does not have yet (downloaded only when a CPU access touches them)
//   hostNewer_      = pages the CPU wrote that the GPU copy does not have yet       (staged + copied before the next GPU draw)
// Env: PS2X_GS_VULKAN=1 enables it; PS2X_GS_COMPARE=1 additionally runs a reference CPU backend and reports per-present
// VRAM mismatches ([vkcmp] lines), then resynchronises the GPU copy to the reference so errors do not accumulate.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_common.h"

// The wrapper needs the CPU backend's CLUT cache (private, and a header edit would rebuild the whole project).
// Hack Ledger: PERF-VK-1 -- `#define private public` around this single include; layout is unchanged.
#define private public
#include "runtime/gs/gs_cpu_backend.h"
#undef private

PresentationFrame ps2x_cpu_present_from_local(GSCpuBackend &b, const GSPresentationRequest &request); // gs_cpu_backend.cpp
namespace GSMem
{
    const uint16_t *ExportPageTable(int idx, size_t *entries);
}

#include "vk/ps2_vk_shaders.inc"
#include "vk/ps2_vk_present_shader.inc" // PERF-PG

#include <atomic>
#include <intrin.h>
static std::atomic<uint64_t> g_gqLockWait{0}, g_gqLockWaits{0}, g_gqPresentHold{0}, g_gqPresents{0}; // PROBE-GQ2
extern "C" uint64_t ps2x_ft2_presentHold() { return g_gqPresentHold.load(std::memory_order_relaxed); } // PERF-LN
extern "C" void ps2_gs_vk_stats(uint64_t *out) { out[0] = g_gqLockWait.exchange(0); out[1] = g_gqLockWaits.exchange(0); out[2] = g_gqPresentHold.exchange(0); out[3] = g_gqPresents.exchange(0); }
extern std::atomic<uint64_t> g_presTicks[8]; // PERF-JD, defined in gs_frontend.cpp
extern "C" void ps2_iu_gpu(uint64_t bytes, uint64_t ticks); // PROBE-IU, defined in gs_frontend.cpp
extern void ps2xRdNoteClut(uint32_t tbp0, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cpsm, uint32_t csa, const uint16_t *clut512); // FEAT-RD3c
extern bool ps2xRdTakeoverOn(); // FEAT-RD12
#include <intrin.h> // PROBE-RDP
extern bool g_rdpOn; extern uint64_t g_rdpCyc[4]; extern uint32_t g_rdpCnt[4]; // PROBE-RDP
struct RdpScopeVk // PROBE-RDP
{
    uint64_t t0;
    RdpScopeVk() : t0(g_rdpOn ? __rdtsc() : 0ull) {} // PROBE-RDP
    ~RdpScopeVk() { if (g_rdpOn) { g_rdpCyc[1] += __rdtsc() - t0; ++g_rdpCnt[1]; } } // PROBE-RDP
};
extern bool g_gwOn; extern "C" int ps2_gw_sw(int cat); // PROBE-GW, defined in gs_frontend.cpp (cats: 6 statechg, 7 vkrec, 8 submit, 9 sync, 10 present)
namespace { struct GwScope { int p; explicit GwScope(int c) : p(g_gwOn ? ps2_gw_sw(c) : -1) {} ~GwScope() { if (p >= 0) ps2_gw_sw(p); } }; } // PROBE-GW

// PROBE-GT: GPU timestamp + present-path accounting (PS2X_GT=1)
static const bool g_gtOn = []() { const char *e = std::getenv("PS2X_GT"); return e && e[0] == '1'; }();
static std::atomic<uint64_t> g_gtFrameNs{0}, g_gtFrameMaxNs{0}, g_gtFrameSamples{0};
static std::atomic<uint64_t> g_gtPresNs{0}, g_gtPresMaxNs{0}, g_gtPresSamples{0};
static std::atomic<uint64_t> g_gtFenceWaitNs{0};
static std::atomic<bool> g_gtTsOk{false};
static std::atomic<uint32_t> g_gtTsPeriodX100{0};
extern "C" bool ps2_gt_enabled() { return g_gtOn; }
extern "C" void ps2_gt_vk_take(double *outFrameAvg, double *outFrameMax, double *outPresAvg, double *outPresMax, double *outFenceMs, bool *outTsOk, double *outTsPeriodNs)
{
    const uint64_t fn = g_gtFrameSamples.exchange(0, std::memory_order_relaxed);
    const uint64_t fs = g_gtFrameNs.exchange(0, std::memory_order_relaxed);
    const uint64_t fm = g_gtFrameMaxNs.exchange(0, std::memory_order_relaxed);
    const uint64_t pn = g_gtPresSamples.exchange(0, std::memory_order_relaxed);
    const uint64_t ps = g_gtPresNs.exchange(0, std::memory_order_relaxed);
    const uint64_t pm = g_gtPresMaxNs.exchange(0, std::memory_order_relaxed);
    const uint64_t fw = g_gtFenceWaitNs.exchange(0, std::memory_order_relaxed);
    *outFrameAvg = fn ? (static_cast<double>(fs) / static_cast<double>(fn) * 1e-6) : 0.0;
    *outFrameMax = static_cast<double>(fm) * 1e-6;
    *outPresAvg = pn ? (static_cast<double>(ps) / static_cast<double>(pn) * 1e-6) : 0.0;
    *outPresMax = static_cast<double>(pm) * 1e-6;
    *outFenceMs = static_cast<double>(fw) * 1e-6;
    *outTsOk = g_gtTsOk.load(std::memory_order_relaxed);
    *outTsPeriodNs = static_cast<double>(g_gtTsPeriodX100.load(std::memory_order_relaxed)) / 100.0;
}

namespace
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
    constexpr uint32_t kPrimStride = 80u; // uints per primitive, shared with the shaders
    constexpr uint32_t kDefaultMaxPrims = 131072u;
    constexpr uint32_t kDefaultMaxClutSlots = 8192u;
    constexpr uint32_t kStagingSize = 32u * 1024u * 1024u;
    constexpr uint32_t kViewport = 2048u;

    // prim field indices (mirror of gs_draw.frag)
    enum : uint32_t
    {
        X0 = 0, Y0 = 3, Z0 = 6, Q0 = 12, S0 = 15, T0 = 18, RGBA0 = 21, UV0 = 24, FOG0 = 27,
        FLAGS = 30, OFX = 31, OFY = 32, SCX0 = 33, SCX1 = 34, SCY0 = 35, SCY1 = 36,
        FBP = 37, FBW = 38, FPSM = 39, FBMSK = 40, ZBP = 41, ZPSM = 42,
        TEST = 43, ALPHA = 44, ALPHAFIX = 45,
        TBP0 = 46, TBW = 47, TPSM = 48, TCC = 49, TFX = 50, CPSM = 51, CSM = 52, CSA = 53,
        TEXW = 54, TEXH = 55, CLAMP_LO = 56, CLAMP_HI = 57, TA0 = 58, AEM = 59, TA1 = 60, FOGRGB = 61,
        BBOX = 62, SPR_UX0 = 66, SPR_UY0 = 67, SPR_SPANX = 68, SPR_SPANY = 69,
        SPR_U0F = 70, SPR_V0F = 71, SPR_U1F = 72, SPR_V1F = 73, SPR_W = 74, SPR_H = 75, SPR_Z = 76, FBA = 77, CLUTSLOT = 78
    };
    enum : uint32_t
    {
        F_SPRITE = 1, F_IIP = 2, F_TME = 4, F_FST = 8, F_FGE = 16, F_ABE = 32, F_PABE = 64, F_LINEAR = 128, F_ZMASK = 256
    };

    uint32_t fbits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    inline int clampInt(int v, int lo, int hi)
    {
        return (v < lo) ? lo : ((v > hi) ? hi : v);
    }

    struct Range
    {
        uint32_t lo = 0xFFFFFFFFu;
        uint32_t hi = 0;
        bool empty() const { return lo >= hi; }
        void add(uint32_t a, uint32_t b)
        {
            if (a >= b)
                return;
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        bool overlaps(uint32_t a, uint32_t b) const { return !empty() && a < hi && b > lo; }
        void clear()
        {
            lo = 0xFFFFFFFFu;
            hi = 0;
        }
    };

    // PERF-HW: page-granular (8 KB GS page, 512 pages in 4 MB) dirty sets replace the single [lo,hi) ranges.
    struct PageSet
    {
        uint64_t w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        bool any() const
        {
            for (uint64_t x : w)
                if (x)
                    return true;
            return false;
        }
        void clear()
        {
            for (uint64_t &x : w)
                x = 0;
        }
        void setAll()
        {
            for (uint64_t &x : w)
                x = ~0ull;
        }
        void set(uint32_t p) { w[(p >> 6) & 7u] |= 1ull << (p & 63u); }
        bool test(uint32_t p) const { return ((w[(p >> 6) & 7u] >> (p & 63u)) & 1ull) != 0ull; }
        void orWith(const PageSet &o)
        {
            for (int i = 0; i < 8; ++i)
                w[i] |= o.w[i];
        }
        void andWith(const PageSet &o)
        {
            for (int i = 0; i < 8; ++i)
                w[i] &= o.w[i];
        }
        void andNot(const PageSet &o)
        {
            for (int i = 0; i < 8; ++i)
                w[i] &= ~o.w[i];
        }
        bool intersects(const PageSet &o) const
        {
            for (int i = 0; i < 8; ++i)
                if (w[i] & o.w[i])
                    return true;
            return false;
        }
    };

    void pageDims(uint32_t psm, uint32_t &pw, uint32_t &ph)
    {
        switch (psm)
        {
        case 2: case 10: case 50: case 58: pw = 64; ph = 64; break;
        case 19: pw = 128; ph = 64; break;
        case 20: pw = 128; ph = 128; break;
        default: pw = 64; ph = 32; break;
        }
    }

    // Conservative byte range of rows [y0,y1] of a surface (base in 256-byte blocks, bw in 64-pixel units).
    void rowsRange(uint32_t baseBlock, uint32_t bw, uint32_t psm, uint32_t y0, uint32_t y1, uint32_t &lo, uint32_t &hi)
    {
        uint32_t pw, ph;
        pageDims(psm, pw, ph);
        const uint32_t pagesPerRow = std::max(1u, (std::max(bw, 1u) * 64u) / pw);
        const uint64_t base = static_cast<uint64_t>(baseBlock) * 256u;
        const uint64_t l = base + static_cast<uint64_t>(y0 / ph) * pagesPerRow * 8192u;
        const uint64_t h = base + static_cast<uint64_t>(y1 / ph + 1u) * pagesPerRow * 8192u;
        if (h > kVramSize || l >= kVramSize)
        {
            lo = 0;
            hi = kVramSize;
            return;
        }
        lo = static_cast<uint32_t>(l);
        hi = static_cast<uint32_t>(h);
    }

    // pages touched by the pixel rectangle [x0,x1]x[y0,y1] of a surface (same page id formula as the swizzle tables)
    void pagesForRect(PageSet &out, uint32_t baseBlock, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
    {
        uint32_t pw, ph;
        pageDims(psm, pw, ph);
        const uint32_t pagesPerRow = std::max(1u, (std::max(bw, 1u) * 64u) / pw);
        const uint32_t basePage = baseBlock / 32u;
        const uint32_t r0 = y0 / ph, r1 = y1 / ph, c0 = x0 / pw, c1 = x1 / pw;
        if (r1 < r0 || c1 < c0 || static_cast<uint64_t>(r1 - r0 + 1u) * (c1 - c0 + 1u) > 4096u)
        {
            out.setAll();
            return;
        }
        for (uint32_t rr = r0; rr <= r1; ++rr)
            for (uint32_t cc = c0; cc <= c1; ++cc)
                out.set((basePage + rr * pagesPerRow + cc) & 511u);
    }

    void pagesForBytes(PageSet &out, uint32_t lo, uint32_t hi)
    {
        if (hi > kVramSize)
            hi = kVramSize;
        for (uint32_t pg = lo >> 13; pg < ((hi + 8191u) >> 13) && pg < 512u; ++pg)
            out.set(pg);
    }

    struct PrimBbox // PERF-GA
    {
        int x0, x1, y0, y1;
    };

    void pagesForRects(PageSet &out, uint32_t baseBlock, uint32_t bw, uint32_t psm, const std::vector<PrimBbox> &bboxes) // PERF-GA
    {
        uint32_t pw, ph;
        pageDims(psm, pw, ph);
        const uint32_t pagesPerRow = std::max(1u, (std::max(bw, 1u) * 64u) / pw);
        const uint32_t basePage = baseBlock / 32u;
        uint32_t prevR0 = ~0u, prevR1 = ~0u, prevC0 = ~0u, prevC1 = ~0u;
        for (const PrimBbox &bb : bboxes)
        {
            const uint32_t r0 = static_cast<uint32_t>(bb.y0) / ph;
            const uint32_t r1 = static_cast<uint32_t>(bb.y1) / ph;
            const uint32_t c0 = static_cast<uint32_t>(bb.x0) / pw;
            const uint32_t c1 = static_cast<uint32_t>(bb.x1) / pw;
            if (r0 == prevR0 && r1 == prevR1 && c0 == prevC0 && c1 == prevC1)
                continue;
            prevR0 = r0;
            prevR1 = r1;
            prevC0 = c0;
            prevC1 = c1;
            if (r1 < r0 || c1 < c0 || static_cast<uint64_t>(r1 - r0 + 1u) * (c1 - c0 + 1u) > 4096u)
            {
                out.setAll();
                return;
            }
            for (uint32_t rr = r0; rr <= r1; ++rr)
                for (uint32_t cc = c0; cc <= c1; ++cc)
                    out.set((basePage + rr * pagesPerRow + cc) & 511u);
        }
    }

    uint32_t popcountPageSet(const PageSet &s) // PERF-GA
    {
        uint32_t c = 0;
        for (uint64_t x : s.w)
        {
            while (x)
            {
                x &= x - 1ull;
                ++c;
            }
        }
        return c;
    }

    bool isFramePsm(uint32_t p) { return p == 0 || p == 1 || p == 2 || p == 10; }
    bool isZPsm(uint32_t p) { return p == 48 || p == 49 || p == 50 || p == 58; }
    bool isTexPsm(uint32_t p)
    {
        return p == 0 || p == 1 || p == 2 || p == 10 || p == 19 || p == 20 || p == 27 || p == 36 || p == 44 || isZPsm(p);
    }
    bool isIndexedPsm(uint32_t p) { return p == 19 || p == 20 || p == 27 || p == 36 || p == 44; }

    float fabsQ(float q) { return (std::fabs(q) > 1.0e-8f) ? q : 1.0f; }

    // ---------------------------------------------------------------- Vulkan context (process singleton)
    struct VkCtx
    {
        bool tried = false;
        bool ok = false;
        VkInstance inst = VK_NULL_HANDLE;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        VkDevice dev = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t qf = 0;
        VkPhysicalDeviceMemoryProperties mem{};
        VkCommandPool pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        bool qCompute = false; // PERF-PG: the chosen queue family can run compute
        float timestampPeriod = 0.0f; // PROBE-GT
        bool timestampOk = false;     // PROBE-GT
        VkBuffer tables = VK_NULL_HANDLE;
        VkDeviceMemory tablesMem = VK_NULL_HANDLE;
        VkDeviceSize tablesSize = 0;
    };
    VkCtx g_vk;

    struct Buf
    {
        VkBuffer b = VK_NULL_HANDLE;
        VkDeviceMemory m = VK_NULL_HANDLE;
        uint8_t *map = nullptr;
        VkDeviceSize size = 0;
    };

    uint32_t findMem(uint32_t typeBits, VkMemoryPropertyFlags want)
    {
        for (uint32_t i = 0; i < g_vk.mem.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (g_vk.mem.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return ~0u;
    }

    bool makeBuf(Buf &out, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool map, VkMemoryPropertyFlags prefer = 0)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        if (vkCreateBuffer(g_vk.dev, &bci, nullptr, &out.b) != VK_SUCCESS)
            return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(g_vk.dev, out.b, &mr);
        uint32_t mt = prefer ? findMem(mr.memoryTypeBits, props | prefer) : ~0u;
        if (mt == ~0u)
            mt = findMem(mr.memoryTypeBits, props);
        if (mt == ~0u)
            return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(g_vk.dev, &mai, nullptr, &out.m) != VK_SUCCESS)
            return false;
        vkBindBufferMemory(g_vk.dev, out.b, out.m, 0);
        out.size = size;
        if (map)
        {
            void *p = nullptr;
            if (vkMapMemory(g_vk.dev, out.m, 0, size, 0, &p) != VK_SUCCESS)
                return false;
            out.map = static_cast<uint8_t *>(p);
        }
        return true;
    }

    bool oneShotCopy(VkBuffer src, VkBuffer dst, VkDeviceSize size)
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_vk.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(g_vk.dev, &ai, &cb) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkBufferCopy c{0, 0, size};
        vkCmdCopyBuffer(cb, src, dst, 1, &c);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        const bool ok = vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(g_vk.queue) == VK_SUCCESS;
        vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
        return ok;
    }

    VkShaderModule makeShader(const uint32_t *code, size_t words)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words * 4;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(g_vk.dev, &ci, nullptr, &m);
        return m;
    }

    bool initVkCtx()
    {
        if (g_vk.tried)
            return g_vk.ok;
        g_vk.tried = true;

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "ps2x";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &g_vk.inst) != VK_SUCCESS)
        {
            std::printf("[vk] instance creation failed\n");
            return false;
        }
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(g_vk.inst, &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n);
        vkEnumeratePhysicalDevices(g_vk.inst, &n, pds.data());
        if (pds.empty())
            return false;
        g_vk.pd = pds[0];
        for (VkPhysicalDevice c : pds)
        {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(c, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                g_vk.pd = c;
                break;
            }
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(g_vk.pd, &props);
        vkGetPhysicalDeviceMemoryProperties(g_vk.pd, &g_vk.mem);

        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.pNext = &il;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(g_vk.pd, &f2);
        if (!il.fragmentShaderPixelInterlock || !f13.dynamicRendering || !f2.features.fragmentStoresAndAtomics || !f2.features.shaderFloat64)
        {
            std::printf("[vk] GPU lacks required features (interlock=%d dynRender=%d fragAtomics=%d fp64=%d)\n",
                        il.fragmentShaderPixelInterlock, f13.dynamicRendering, f2.features.fragmentStoresAndAtomics, f2.features.shaderFloat64);
            return false;
        }
        // enable only what we need
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        il2.fragmentShaderPixelInterlock = VK_TRUE;
        VkPhysicalDeviceVulkan13Features f13b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13b.dynamicRendering = VK_TRUE;
        f13b.pNext = &il2;
        VkPhysicalDeviceFeatures2 f2b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2b.features.fragmentStoresAndAtomics = VK_TRUE;
        f2b.features.shaderFloat64 = VK_TRUE;
        f2b.pNext = &f13b;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &qn, qfs.data());
        g_vk.qf = ~0u;
        for (uint32_t pass = 0; pass < 2 && g_vk.qf == ~0u; ++pass) // PERF-PG: prefer a graphics family that can also run compute
            for (uint32_t i = 0; i < qn; ++i)
                if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (pass == 1 || (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)))
                {
                    g_vk.qf = i;
                    g_vk.qCompute = (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                    break;
                }
        if (g_vk.qf == ~0u)
            return false;
        g_vk.timestampPeriod = props.limits.timestampPeriod; // PROBE-GT
        g_vk.timestampOk = (props.limits.timestampPeriod > 0.0f && qfs[g_vk.qf].timestampValidBits > 0);
        g_gtTsOk.store(g_vk.timestampOk, std::memory_order_relaxed);
        g_gtTsPeriodX100.store(static_cast<uint32_t>(g_vk.timestampPeriod * 100.0f + 0.5f), std::memory_order_relaxed);
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = g_vk.qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char *exts[] = {VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f2b;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = exts;
        if (vkCreateDevice(g_vk.pd, &dci, nullptr, &g_vk.dev) != VK_SUCCESS)
        {
            std::printf("[vk] device creation failed\n");
            return false;
        }
        vkGetDeviceQueue(g_vk.dev, g_vk.qf, 0, &g_vk.queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = g_vk.qf;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(g_vk.dev, &pci, nullptr, &g_vk.pool) != VK_SUCCESS)
            return false;

        // page tables -> device-local SSBO (order must match gs_draw.frag T_* offsets)
        size_t total = 0;
        std::vector<std::pair<const uint16_t *, size_t>> tabs;
        for (int i = 0; i < 8; ++i)
        {
            size_t e = 0;
            const uint16_t *p = GSMem::ExportPageTable(i, &e);
            if (!p || e == 0)
            {
                std::printf("[vk] page table %d missing\n", i);
                return false;
            }
            tabs.emplace_back(p, e);
            total += e;
        }
        if (total != 1441792u)
        {
            std::printf("[vk] page table total %zu != 1441792 (shader offsets out of sync)\n", total);
            return false;
        }
        g_vk.tablesSize = total * 2u;
        Buf stage;
        if (!makeBuf(stage, g_vk.tablesSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true))
            return false;
        size_t off = 0;
        for (auto &t : tabs)
        {
            std::memcpy(stage.map + off * 2u, t.first, t.second * 2u);
            off += t.second;
        }
        Buf tb;
        if (!makeBuf(tb, g_vk.tablesSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false))
            return false;
        if (!oneShotCopy(stage.b, tb.b, g_vk.tablesSize))
            return false;
        vkDestroyBuffer(g_vk.dev, stage.b, nullptr);
        vkFreeMemory(g_vk.dev, stage.m, nullptr);
        g_vk.tables = tb.b;
        g_vk.tablesMem = tb.m;

        // descriptor set layout: 0 vram, 1 tables, 2 clut, 3 prims
        VkDescriptorSetLayoutBinding bnd[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            bnd[i].binding = i;
            bnd[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bnd[i].descriptorCount = 1;
            bnd[i].stageFlags = (i == 3) ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dsl.bindingCount = 4;
        dsl.pBindings = bnd;
        if (vkCreateDescriptorSetLayout(g_vk.dev, &dsl, nullptr, &g_vk.setLayout) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &g_vk.setLayout;
        if (vkCreatePipelineLayout(g_vk.dev, &plci, nullptr, &g_vk.pipeLayout) != VK_SUCCESS)
            return false;

        g_vk.vs = makeShader(kVertSpv, sizeof(kVertSpv) / 4);
        g_vk.fs = makeShader(kFragSpv, sizeof(kFragSpv) / 4);
        if (!g_vk.vs || !g_vk.fs)
            return false;

        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = g_vk.vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = g_vk.fs;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rc{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &rc;
        gp.stageCount = 2;
        gp.pStages = st;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &ds;
        gp.layout = g_vk.pipeLayout;
        const auto t0 = std::chrono::steady_clock::now();
        if (vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &g_vk.pipeline) != VK_SUCCESS)
        {
            std::printf("[vk] pipeline creation failed\n");
            return false;
        }
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[vk] GS backend ready on %s (pipeline %.0f ms)\n", props.deviceName, ms2);
        g_vk.ok = true;
        return true;
    }

    // PERF-PG: what the GPU display conversion has to read (see GSVulkanBackend::planGpuPresent)
    struct GpuPlan
    {
        bool ok = false, usedPreferred = false, fieldMode = false, oddField = false;
        uint32_t base = 0, fbw = 0, psm = 0, x0 = 0, y0 = 0, w = 0, h = 0, displayFbp = 0, sourceFbp = 0;
        // both display circuits blended (PMODE): circuit 2's frame and the blend controls; outW/outH = size of the blended picture
        bool dual = false;
        uint32_t flags = 0, base2 = 0, fbw2 = 0, psm2 = 0, x2 = 0, y2 = 0, w2 = 0, h2 = 0, bg = 0, outW = 0, outH = 0;
    };

    // ---------------------------------------------------------------- the backend
    class GSVulkanBackend final : public GSRasterBackend
    {
    public:
        GSVulkanBackend()
            : cpu_(std::make_unique<GSCpuBackend>())
        {
            const char *c = std::getenv("PS2X_GS_COMPARE");
            compare_ = c && c[0] == '1';
            const char *o = std::getenv("PS2X_GS_VK_MAXPRIM"); // debug bisect: only the first N GPU prims per frame
            maxPrimPerFrame_ = o ? static_cast<uint32_t>(std::strtoul(o, nullptr, 10)) : 0u;
            const char *stg = std::getenv("PS2X_VK_PRIMSTAGE");
            primStage_ = !(stg && stg[0] == '0');
            const char *ga = std::getenv("PS2X_GA_CHECK"); // PERF-GA
            gaCheck_ = ga && ga[0] == '1';
            gaLastPrint_ = std::chrono::steady_clock::now();
            if (compare_)
                ref_ = std::make_unique<GSCpuBackend>();

            const char *ep = std::getenv("PS2X_VK_MAXPRIMS");
            if (ep && *ep)
            {
                unsigned long v = std::strtoul(ep, nullptr, 10);
                maxPrims_ = std::clamp<uint32_t>(static_cast<uint32_t>(v), 24576u, 262144u);
            }
            const char *ec = std::getenv("PS2X_VK_MAXCLUT");
            if (ec && *ec)
            {
                unsigned long v = std::strtoul(ec, nullptr, 10);
                maxClutSlots_ = std::clamp<uint32_t>(static_cast<uint32_t>(v), 2048u, 16384u);
            }
            if (maxPrims_ != kDefaultMaxPrims || maxClutSlots_ != kDefaultMaxClutSlots)
                std::printf("[vk] capacities prims=%u clut=%u\n", maxPrims_, maxClutSlots_);
            (void)ps2xRdTakeoverOn(); // FEAT-RD12: log startup banner once
        }
        ~GSVulkanBackend() override // PROBE-GT
        {
            if (gtQueryPool_ != VK_NULL_HANDLE && g_vk.dev != VK_NULL_HANDLE)
            {
                vkDeviceWaitIdle(g_vk.dev);
                vkDestroyQueryPool(g_vk.dev, gtQueryPool_, nullptr);
                gtQueryPool_ = VK_NULL_HANDLE;
            }
        }

        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            std::lock_guard<std::mutex> lk(m_);
            host_ = vram;
            hostSize_ = vramSize;
            cpu_->Initialize(vram, vramSize);
            if (ref_)
            {
                refVram_.assign(vramSize, 0);
                ref_->Initialize(refVram_.data(), vramSize);
            }
            if (!vkReady_ && !vkFailed_)
                vkReady_ = createResources();
            if (!vkReady_)
                vkFailed_ = true;
            gpuColor_.clear();
            gpuZ_.clear();
            hostNewer_.setAll();
        }

        void Reset() override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (vkReady_)
                discardGpu();
            cpu_->Reset();
            if (ref_)
                ref_->Reset();
            hostNewer_.setAll();
            clutChanged_ = true;
        }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            const uint64_t gqL0 = __rdtsc(); // PROBE-GQ2
            std::lock_guard<std::mutex> lk(m_);
            g_gqLockWait.fetch_add(__rdtsc() - gqL0, std::memory_order_relaxed);
            g_gqLockWaits.fetch_add(1, std::memory_order_relaxed);
            if (ref_)
                ref_->Submit(batch);
            if (ps2xRdTakeoverOn()) // FEAT-RD12: skip GPU + CPU raster, keep CLUT notes
            {
                if (clutChanged_ || curClutSlot_ < 0)
                {
                    curClutSlot_ = (curClutSlot_ + 1) & 0x7FFFFFFF;
                    clutChanged_ = false;
                }
                noteRdClut(batch.state);
                return;
            }
            if (!vkReady_ || batch.vertexCount == 0u)
            {
                cpuDraw(batch);
                return;
            }
            const int reason = gpuDraw(batch);
            if (reason > 0)
            {
                ++fallback_[std::min(reason, 15)];
                cpuDraw(batch);
            }
        }

        void submitRunPerPrim(const GSDrawState &state, const GSRunPrim *prims, size_t count) // PERF-GA
        {
            GSPrimitiveBatch batch;
            batch.state = state;
            for (size_t i = 0; i < count; ++i)
            {
                batch.vertices = prims[i].vertices;
                batch.vertexCount = prims[i].vertexCount;
                if (ref_)
                    ref_->Submit(batch);
                if (!vkReady_ || batch.vertexCount == 0u)
                {
                    cpuDraw(batch);
                    continue;
                }
                const int reason = gpuDraw(batch);
                if (reason > 0)
                {
                    ++fallback_[std::min(reason, 15)];
                    cpuDraw(batch);
                }
            }
        }

        void SubmitRun(const GSDrawState &state, const GSRunPrim *prims, size_t count) override // PERF-GR
        {
            GwScope gwS(7); // PROBE-GW vkrec (SubmitRun incl. lock wait + gpuDraw recording; flush() inside re-scopes submit/sync)
            const uint64_t gqL0 = __rdtsc(); // PROBE-GQ2
            std::lock_guard<std::mutex> lk(m_);
            g_gqLockWait.fetch_add(__rdtsc() - gqL0, std::memory_order_relaxed);
            g_gqLockWaits.fetch_add(1, std::memory_order_relaxed);
            if (count == 0u)
                return;
            if (ps2xRdTakeoverOn()) // FEAT-RD12: skip GPU + CPU raster, keep CLUT notes
            {
                if (clutChanged_ || curClutSlot_ < 0)
                {
                    curClutSlot_ = (curClutSlot_ + 1) & 0x7FFFFFFF;
                    clutChanged_ = false;
                }
                noteRdClut(state);
                return;
            }
            // PERF-GA: fall back to per-prim when Vulkan is not ready, compare/ref is active,
            // run would cross ring/frame prim limits, or any prim has an invalid vertex count.
            if (!vkReady_ || compare_ || ref_ != nullptr ||
                primCount_ + count > maxPrims_ ||
                (maxPrimPerFrame_ && primsThisFrame_ + count > maxPrimPerFrame_))
            {
                submitRunPerPrim(state, prims, count);
                return;
            }
            const uint32_t minVerts = (state.prim.type == GS_PRIM_SPRITE) ? 2u : 3u;
            for (size_t i = 0; i < count; ++i)
            {
                if (prims[i].vertexCount < minVerts)
                {
                    submitRunPerPrim(state, prims, count);
                    return;
                }
            }

            alignas(64) uint32_t tmpl[kPrimStride];
            uint32_t tlo = 0, thi = 0;
            bool sprite = false;
            const int reason = runBegin(state, tlo, thi, sprite, tmpl);
            if (reason > 0)
            {
                submitRunPerPrim(state, prims, count);
                return;
            }

            runBboxes_.clear();
            uint32_t runPrims = 0;
            int uMinX = 0x7FFFFFFF, uMaxX = -0x7FFFFFFF, uMinY = 0x7FFFFFFF, uMaxY = -0x7FFFFFFF;
            uint64_t sumPrimArea = 0;
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t *dst = primMap_ + static_cast<size_t>(primCount_ + runPrims) * kPrimStride;
                int bminX, bmaxX, bminY, bmaxY;
                if (fillPrimFields(state, prims[i].vertices.data(),prims[i].vertexCount, sprite, tmpl, dst, bminX, bmaxX, bminY, bmaxY))
                {
                    if (bminX < uMinX) uMinX = bminX;
                    if (bmaxX > uMaxX) uMaxX = bmaxX;
                    if (bminY < uMinY) uMinY = bminY;
                    if (bmaxY > uMaxY) uMaxY = bmaxY;
                    sumPrimArea += static_cast<uint64_t>(bmaxX - bminX + 1) * static_cast<uint64_t>(bmaxY - bminY + 1);
                    runBboxes_.push_back({bminX, bmaxX, bminY, bmaxY});
                    ++runPrims;
                }
            }
            if (runPrims == 0u)
                return;

            const GSContext &ctx = state.context;
            uint32_t flo, fhi, zlo, zhi;
            rowsRange(ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, uMinY, uMaxY, flo, fhi);
            rowsRange(ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, uMinY, uMaxY, zlo, zhi);
            // PERF-GA: self-dependency check (sampled texture byte range overlaps run's own frame or Z range)
            if (runPrims > 1u && state.prim.tme && ((tlo < fhi && thi > flo) || (tlo < zhi && thi > zlo)))
            {
                submitRunPerPrim(state, prims, count);
                return;
            }

            runEnd(state, prims, count, sprite, tmpl, runPrims, uMinX, uMaxX, uMinY, uMaxY, sumPrimArea, tlo, thi, flo, fhi, zlo, zhi);
        }

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            GwScope gwS(6); // PROBE-GW statechg
            const uint64_t gqL0 = __rdtsc(); // PROBE-GQ2
            std::lock_guard<std::mutex> lk(m_);
            g_gqLockWait.fetch_add(__rdtsc() - gqL0, std::memory_order_relaxed);
            g_gqLockWaits.fetch_add(1, std::memory_order_relaxed);
            if (ref_)
                ref_->LoadClut(tex0, texclut);
            if (vkReady_)
            {
                // mirror of GSCpuBackend::LoadClut's early-outs: most calls read nothing
                const uint32_t tp = tex0.psm;
                const bool indexed = tp == 19 || tp == 20 || tp == 27 || tp == 36 || tp == 44;
                bool willRead = false;
                if (indexed)
                {
                    switch (tex0.cld)
                    {
                    case 1u: case 2u: case 3u: willRead = true; break;
                    case 4u: willRead = (cpu_->m_clutCbp[0] != tex0.cbp); break;
                    case 5u: willRead = (cpu_->m_clutCbp[1] != tex0.cbp); break;
                    default: break;
                    }
                }
                uint32_t effCbp = 0;
                if (willRead)
                {
                    effCbp = tex0.cbp != 0u ? tex0.cbp : ((tex0.cld == 2u || tex0.cld == 4u) ? 0u : cpu_->m_clutCbp[0]);
                    if (effCbp == 0u)
                        willRead = false;
                }
                if (willRead)
                {
                    PageSet need;
                    if (tex0.csm == 0u)
                        pagesForBytes(need, effCbp * 256u, std::min(effCbp * 256u + 8192u, kVramSize)); // 16x16 CSM1 block, may straddle a page
                    else
                        pagesForRect(need, effCbp, std::max<uint32_t>(texclut.cbw, 1u), tex0.cpsm, texclut.cou * 16u, texclut.cou * 16u + 255u, texclut.cov, texclut.cov);
                    {
                        PageSet ov = gpuColor_;
                        ov.orWith(gpuZ_);
                        ov.andWith(need);
                        if (ov.any())
                        {
                            uint32_t pg = 0;
                            while (!ov.test(pg))
                                ++pg;
                            const uint32_t key = (pg << 2) | (gpuColor_.test(pg) ? 1u : 0u) | (gpuZ_.test(pg) ? 2u : 0u);
                            ++clutDiag_[key];
                        }
                    }
                    syncPages(need, 2);
                }
            }
            cpu_->LoadClut(tex0, texclut);
            clutChanged_ = true;
        }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            GwScope gwS(6); // PROBE-GW statechg (transfer setup; its flush/sync re-scope)
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->BeginTransfer(command);
            if (vkReady_)
            {
                const auto &bb = command.bitbltbuf;
                const auto &tp = command.trxpos;
                const auto &tr = command.trxreg;
                const uint32_t dir = command.direction;
                const uint32_t rw = std::max<uint32_t>(tr.rrw, 1u), rh = std::max<uint32_t>(tr.rrh, 1u);
                if (dir == 0u || dir == 2u) // host->local, local->local: destination written
                {
                    PageSet pg;
                    pagesForRect(pg, bb.dbp, bb.dbw, bb.dpsm, tp.dsax, tp.dsax + rw - 1u, tp.dsay, tp.dsay + rh - 1u);
                    syncPages(pg, 1);
                    hostNewer_.orWith(pg);
                }
                if (dir == 1u || dir == 2u) // local->host, local->local: source read
                {
                    PageSet pg;
                    pagesForRect(pg, bb.sbp, bb.sbw, bb.spsm, tp.ssax, tp.ssax + rw - 1u, tp.ssay, tp.ssay + rh - 1u);
                    syncPages(pg, 1);
                }
            }
            cpu_->BeginTransfer(command);
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->UploadImage(data, sizeBytes);
            cpu_->UploadImage(data, sizeBytes); // destination range was marked dirty at BeginTransfer
        }

        void Flush() override
        {
            GwScope gwS(6); // PROBE-GW
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->Flush();
            cpu_->Flush();
        }

        void TextureFlush() override
        {
            GwScope gwS(6); // PROBE-GW
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->TextureFlush();
            cpu_->TextureFlush();
        }

        void Sync(GSSyncReason reason) override
        {
            GwScope gwS(9); // PROBE-GW
            std::lock_guard<std::mutex> lk(m_);
            // PERF-HX: Finish/Presentation/LocalToHost need no download here (Present and BeginTransfer sync the pages they read)
            if (vkReady_ && (reason == GSSyncReason::DebugReadback || reason == GSSyncReason::Reset))
                syncAll(6);
            if (ref_)
                ref_->Sync(reason);
            cpu_->Sync(reason);
        }

        // PERF-JD: PS2X_PRESENT_NARROW=1 downloads only the pages the CPU presenter can read (display frames of both CRTs, the two context frames and
        // the preferred source) instead of every GPU-newer colour page. Rows are bounded by the display height (DH+1), widths by FBW*64.
        static bool presentNarrow()
        {
            static const bool on = []() { const char *e = std::getenv("PS2X_PRESENT_NARROW"); return e && e[0] == '1'; }();
            return on;
        }

        static void addPresentPages(PageSet &out, const GSPresentationRequest &rq)
        {
            auto rowsOf = [](uint64_t display64) -> uint32_t {
                const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu) + 1u;
                return std::min(dh, 1024u);
            };
            const uint32_t rows = std::max(448u, std::max(rowsOf(rq.display1), rowsOf(rq.display2)));
            auto addFrame = [&](uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t dby) {
                if (fbw == 0u)
                    return;
                pagesForRect(out, fbp << 5u, fbw, psm, 0u, fbw * 64u - 1u, dby, dby + rows - 1u);
            };
            for (uint64_t d : {rq.dispfb1, rq.dispfb2})
                addFrame(static_cast<uint32_t>(d & 0x1FFu), static_cast<uint32_t>((d >> 9) & 0x3Fu), static_cast<uint32_t>((d >> 15) & 0x1Fu),
                         static_cast<uint32_t>((d >> 43) & 0x7FFu));
            for (const GSFrameReg &f : rq.contextFrames)
                addFrame(f.fbp, f.fbw, f.psm, 0u);
            if (rq.hasPreferredSource)
                addFrame(rq.preferredSource.fbp, rq.preferredSource.fbw, rq.preferredSource.psm, 0u);
        }

        // PERF-AP: wait for the in-flight present copy and move its pages into the private VRAM image
        bool completeAsync()
        {
            if (!asyncPending_)
                return false;
            GwScope gwS(9); // PROBE-GW sync (present fence wait)
            // PERF-PG: PS2X_FENCE_POLL=1 sleeps between status checks instead of blocking in vkWaitForFences (drivers may spin there and bill the CPU)
            static const bool s_poll = []() { const char *e = std::getenv("PS2X_FENCE_POLL"); return e && e[0] == '1'; }();
            const uint64_t fw0 = __rdtsc();
            const auto gtF0 = g_gtOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; // PROBE-GT
            VkResult r;
            if (s_poll)
            {
                while ((r = vkGetFenceStatus(g_vk.dev, alt_.fence)) == VK_NOT_READY)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            else
                r = vkWaitForFences(g_vk.dev, 1, &alt_.fence, VK_TRUE, ~0ull);
            if (g_gtOn) // PROBE-GT
                g_gtFenceWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gtF0).count()), std::memory_order_relaxed);
            g_presTicks[4].fetch_add(__rdtsc() - fw0, std::memory_order_relaxed); // shown as "snapshot" in the [present] line: fence wait
            vkResetFences(g_vk.dev, 1, &alt_.fence);
            asyncPending_ = false;
            if (r != VK_SUCCESS)
            {
                vkReady_ = false;
                vkFailed_ = true;
                return false;
            }
            if (asyncKind_ & 1)
            {
                std::vector<std::pair<uint32_t, uint32_t>> runs;
                pageRuns(asyncPages_, runs);
                for (const auto &run : runs)
                {
                    const size_t off = static_cast<size_t>(run.first) * 8192u, len = static_cast<size_t>(run.second) * 8192u;
                    std::memcpy(presVram_.data() + off, snap_.map + off, len);
                    downloadBytes_ += len;
                }
            }
            return true;
        }

        // PERF-AP: queue the copy of the display pages (vram_ is authoritative after ensureUpload) without waiting for the GPU.
        // PERF-PG: with a plan the display conversion is dispatched on the GPU as well; pages=false skips the page copy (and leaves presDirty_ alone).
        void startAsync(const GSPresentationRequest &request, const GpuPlan *gp = nullptr, bool pages = true)
        {
            if (!ensureUpload())
                return;
            PageSet need;
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            if (pages)
            {
                addPresentPages(need, request);
                need.orWith(presDirty_); // the CPU presenter may read any colour page: keep presVram_ equal to vram_, not just the display pages
                presDirty_.clear();
                pageRuns(need, runs);
            }
            gtInStartAsync_ = true; // PROBE-GT
            beginRecording();
            gtInStartAsync_ = false; // PROBE-GT
            endPass();
            bool gtPresActive = false; // PROBE-GT
            uint32_t gtPresSlot = 0;   // PROBE-GT
            if (gtQueryPool_ != VK_NULL_HANDLE) // PROBE-GT
            {
                gtHarvestQueries();
                if ((gtPresWriteIdx_ - gtPresReadIdx_) < kGtRing)
                {
                    gtPresSlot = static_cast<uint32_t>(gtPresWriteIdx_ % kGtRing);
                    vkCmdResetQueryPool(cb_, gtQueryPool_, kGtRing * 2u + gtPresSlot * 2u, 2u);
                    vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gtQueryPool_, kGtRing * 2u + gtPresSlot * 2u);
                    gtPresActive = true;
                }
            }
            if (pages)
            {
                std::vector<VkBufferCopy> regions;
                for (const auto &run : runs)
                    regions.push_back(VkBufferCopy{static_cast<VkDeviceSize>(run.first) * 8192u, static_cast<VkDeviceSize>(run.first) * 8192u,
                                                   static_cast<VkDeviceSize>(run.second) * 8192u});
                bufBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                if (!regions.empty())
                    vkCmdCopyBuffer(cb_, vram_.b, snap_.b, static_cast<uint32_t>(regions.size()), regions.data());
                VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.buffer = snap_.b;
                hb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
            }
            if (gp)
                recordGpuConvert(*gp);
            if (gtPresActive) // PROBE-GT
            {
                vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gtQueryPool_, kGtRing * 2u + gtPresSlot * 2u + 1u);
                ++gtPresWriteIdx_;
            }
            vkEndCommandBuffer(cb_);
            recording_ = false;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb_;
            GwScope gwSub(8); // PROBE-GW submit (async present copy)
            if (vkQueueSubmit(g_vk.queue, 1, &si, fence_) == VK_SUCCESS)
            {
                asyncPending_ = true;
                asyncPages_ = need;
                asyncReq_ = request;
                asyncKind_ = (pages ? 1 : 0) | (gp ? 2 : 0);
                gpuPlan_ = gp ? *gp : GpuPlan{};
                orderAfterAsync_ = true;
                ++submits_;
                resetRings();
                clutChanged_ = true;
                swapSlot();
            }
            else
            {
                vkResetFences(g_vk.dev, 1, &fence_);
                vkReady_ = false;
                vkFailed_ = true;
                resetRings();
            }
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            GwScope gwS(10); // PROBE-GW present
            if (ps2xRdTakeoverOn()) // FEAT-RD12: skip GS frame readback/conversion
            {
                std::lock_guard<std::mutex> lk(m_);
                ++presents_;
                primsThisFrame_ = 0;
                return {};
            }
            if (asyncOk_ && asyncPresentWanted() && !compare_ && !ref_)
            {
                const uint64_t lk0 = __rdtsc();
                std::unique_lock<std::mutex> lk(m_);
                g_presTicks[5].fetch_add(__rdtsc() - lk0, std::memory_order_relaxed); // shown as "deswizzle": wait for the backend mutex
                if (vkReady_)
                {
                    const uint64_t gqP0 = __rdtsc(), pj0 = gqP0;
                    const int doneKind = asyncKind_;
                    const GpuPlan donePlan = gpuPlan_;
                    const bool haveSnap = completeAsync();
                    const GSPresentationRequest doneReq = asyncReq_;
                    PresentationFrame gpuFrame;
                    bool gpuNeedCpu = false;
                    if (haveSnap && (doneKind & 2))
                    {
                        readGpuFrame(donePlan, gpuFrame, gpuNeedCpu);
                        if (gpuNeedCpu)
                        {
                            ++pgBlack_;
                            syncPages(gpuColor_, 0); // (nearly) black frame: the presenter tries the other context frames, which only the CPU path knows
                        }
                        else
                            ++pgGpu_;
                    }
                    else if (haveSnap)
                        ++pgPages_;
                    if (vkReady_ && !haveSnap)
                    {
                        PageSet first; // first present: the synchronous path supplies this frame
                        addPresentPages(first, request);
                        syncPages(first, 0);
                    }
                    if (vkReady_)
                    {
                        GpuPlan plan;
                        uint32_t why = 0;
                        const bool useGpu = gpuPresOk_ && planGpuPresent(request, plan, why);
                        if (gpuPresOk_ && !useGpu)
                            ++pgIneligible_[why & 3u];
                        startAsync(request, useGpu ? &plan : nullptr, !useGpu || gpuVerify_);
                    }
                    g_presTicks[3].fetch_add(__rdtsc() - pj0, std::memory_order_relaxed);
                    if (gtQueryPool_ != VK_NULL_HANDLE) gtHarvestQueries(); // PROBE-GT
                    ++presents_;
                    maybePrintStats();
                    if (gpuPresOk_ && presents_ % 240u == 0u)
                        std::printf("[presgpu] gpu=%llu pages=%llu blackFallback=%llu ineligible(none/both/format)=%llu/%llu/%llu verify: frames=%llu mismatched=%llu\n",
                                    static_cast<unsigned long long>(pgGpu_), static_cast<unsigned long long>(pgPages_), static_cast<unsigned long long>(pgBlack_),
                                    static_cast<unsigned long long>(pgIneligible_[0]), static_cast<unsigned long long>(pgIneligible_[1]),
                                    static_cast<unsigned long long>(pgIneligible_[2]), static_cast<unsigned long long>(pgVerified_), static_cast<unsigned long long>(pgMismatch_));
                    primsThisFrame_ = 0;
                    g_gqPresentHold.fetch_add(__rdtsc() - gqP0, std::memory_order_relaxed);
                    g_gqPresents.fetch_add(1, std::memory_order_relaxed);
                    lk.unlock();
                    if (haveSnap)
                    {
                        if ((doneKind & 2) && !gpuNeedCpu)
                        {
                            if (doneKind & 1) // verify: convert the same request from the page copy and compare
                            {
                                if (!presCpu_)
                                    presCpu_ = std::make_unique<GSCpuBackend>();
                                presCpu_->Initialize(presVram_.data(), static_cast<uint32_t>(presVram_.size()));
                                const PresentationFrame ref = ps2x_cpu_present_from_local(*presCpu_, doneReq);
                                ++pgVerified_;
                                bool same = ref.width == gpuFrame.width && ref.height == gpuFrame.height && ref.displayFbp == gpuFrame.displayFbp &&
                                            ref.sourceFbp == gpuFrame.sourceFbp && ref.usedPreferred == gpuFrame.usedPreferred;
                                uint32_t badPx = 0, fx = 0, fy = 0;
                                if (same)
                                    for (uint32_t y = 0; y < ref.height; ++y)
                                        for (uint32_t x = 0; x < ref.width; ++x)
                                            if (std::memcmp(ref.pixels.data() + (static_cast<size_t>(y) * 640u + x) * 4u, gpuFrame.pixels.data() + (static_cast<size_t>(y) * 640u + x) * 4u, 4) != 0)
                                            {
                                                if (badPx++ == 0)
                                                {
                                                    fx = x;
                                                    fy = y;
                                                }
                                            }
                                if (!same || badPx)
                                {
                                    if (pgMismatch_++ < 8)
                                        std::printf("[presgpu] MISMATCH dims gpu=%ux%u ref=%ux%u fbp gpu=%u/%u ref=%u/%u pref %d/%d differingPixels=%u first=(%u,%u)\n", gpuFrame.width, gpuFrame.height,
                                                    ref.width, ref.height, gpuFrame.displayFbp, gpuFrame.sourceFbp, ref.displayFbp, ref.sourceFbp, gpuFrame.usedPreferred ? 1 : 0,
                                                    ref.usedPreferred ? 1 : 0, badPx, fx, fy);
                                }
                            }
                            return gpuFrame;
                        }
                        if (doneKind & 2) // black-frame fallback: the old path on the current VRAM
                            return cpu_->Present(doneReq);
                        if (!presCpu_)
                            presCpu_ = std::make_unique<GSCpuBackend>();
                        presCpu_->Initialize(presVram_.data(), static_cast<uint32_t>(presVram_.size()));
                        return ps2x_cpu_present_from_local(*presCpu_, doneReq);
                    }
                    return cpu_->Present(request);
                }
            }
            std::unique_lock<std::mutex> lk(m_);
            const uint64_t gqP0 = __rdtsc(); // PROBE-GQ2: time the lock is held
            if (vkReady_)
            {
                // the CPU presenter only reads colour surfaces; Z pages stay GPU-newer unless the compare mode needs everything
                const uint64_t pj0 = __rdtsc(); // PERF-JD
                if (compare_)
                    syncAll(0);
                else if (presentNarrow())
                {
                    PageSet need;
                    addPresentPages(need, request);
                    syncPages(need, 0);
                }
                else
                    syncPages(gpuColor_, 0);
                g_presTicks[3].fetch_add(__rdtsc() - pj0, std::memory_order_relaxed);
            }
            if (ref_)
                comparePresent();
            ++presents_;
            maybePrintStats();
            primsThisFrame_ = 0;
            g_gqPresentHold.fetch_add(__rdtsc() - gqP0, std::memory_order_relaxed); g_gqPresents.fetch_add(1, std::memory_order_relaxed); // PROBE-GQ2
            lk.unlock(); // PERF-PU: the VRAM snapshot + display conversion run under the CPU backend's own mutex; the draw thread no longer waits for them
            return cpu_->Present(request);
        }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->ClearFramebuffer(context, rgba);
            if (vkReady_)
            {
                PageSet pg;
                pagesForRect(pg, context.frame.fbp << 5u, context.frame.fbw, context.frame.psm, 0, std::max(context.frame.fbw, 1u) * 64u - 1u, 0,
                             std::min<uint32_t>(context.scissor.y1, 2047u));
                syncPages(pg, 3);
                hostNewer_.orWith(pg);
            }
            return cpu_->ClearFramebuffer(context, rgba);
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
            {
                std::vector<uint8_t> tmp(maxBytes);
                ref_->ConsumeLocalToHostBytes(tmp.data(), maxBytes);
            }
            return cpu_->ConsumeLocalToHostBytes(dst, maxBytes);
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            auto *self = const_cast<GSVulkanBackend *>(this);
            std::lock_guard<std::mutex> lk(self->m_);
            if (self->vkReady_)
            {
                PageSet pg;
                pagesForRect(pg, base, bw, psm, x, x, y, y);
                self->syncPages(pg, 5);
            }
            return cpu_->ReadVram(psm, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->WriteVram(psm, base, bw, x, y, value);
            if (vkReady_)
            {
                PageSet pg;
                pagesForRect(pg, base, bw, psm, x, x, y, y);
                syncPages(pg, 4);
                hostNewer_.orWith(pg);
            }
            cpu_->WriteVram(psm, base, bw, x, y, value);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            auto *self = const_cast<GSVulkanBackend *>(this);
            std::lock_guard<std::mutex> lk(self->m_);
            if (self->vkReady_)
                self->syncAll(5);
            cpu_->SnapshotVram(out);
        }

        GSTransferSnapshot GetTransferSnapshot() const override { return cpu_->GetTransferSnapshot(); }

    private:
        // ------------------------------------------------------------ resources
        bool createResources()
        {
            if (!initVkCtx())
                return false;
            const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if (!makeBuf(vram_, kVramSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false))
                return false;
            if (!makeBuf(staging_, kStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, true))
                return false;
            if (!makeBuf(down_, kVramSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, true, VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                return false;
            if (!makeBuf(prims_, static_cast<VkDeviceSize>(maxPrims_) * kPrimStride * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true))
                return false;
            if (!makeBuf(clutBuf_, static_cast<VkDeviceSize>(maxClutSlots_) * 1024u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true))
                return false;

            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 2;
            dpi.poolSizeCount = 1;
            dpi.pPoolSizes = &ps;
            if (vkCreateDescriptorPool(g_vk.dev, &dpi, nullptr, &descPool_) != VK_SUCCESS)
                return false;
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = descPool_;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &g_vk.setLayout;
            if (vkAllocateDescriptorSets(g_vk.dev, &dai, &desc_) != VK_SUCCESS)
                return false;
            VkDescriptorBufferInfo bi[4] = {{vram_.b, 0, VK_WHOLE_SIZE}, {g_vk.tables, 0, VK_WHOLE_SIZE}, {clutBuf_.b, 0, VK_WHOLE_SIZE}, {prims_.b, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[4]{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = desc_;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bi[i];
            }
            vkUpdateDescriptorSets(g_vk.dev, 4, w, 0, nullptr);

            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = g_vk.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(g_vk.dev, &cai, &cb_) != VK_SUCCESS)
                return false;
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateFence(g_vk.dev, &fci, nullptr, &fence_) != VK_SUCCESS)
                return false;
            primMap_ = reinterpret_cast<uint32_t *>(prims_.map);
            if (asyncPresentWanted())
                asyncOk_ = createAltSlot(hv);
            if (asyncOk_ && gpuPresentWanted())
                gpuPresOk_ = createPresentGpu();
            if (g_gtOn && g_vk.timestampOk) // PROBE-GT
            {
                VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qpi.queryCount = kGtRing * 4u; // [0..15] GS CB, [16..31] present/composite
                if (vkCreateQueryPool(g_vk.dev, &qpi, nullptr, &gtQueryPool_) != VK_SUCCESS)
                    gtQueryPool_ = VK_NULL_HANDLE;
            }
            return true;
        }

        // PERF-AP: PS2X_PRESENT_ASYNC=1 -- present submits the display-page copy without waiting; the converted frame is returned one present later.
        // The ring buffers, descriptor set, command buffer and fence exist twice; the slot that is in flight is always alt_.
        static bool asyncPresentWanted()
        {
            static const bool on = []() { const char *e = std::getenv("PS2X_PRESENT_ASYNC"); return e && e[0] == '1'; }();
            return on;
        }

        bool createAltSlot(VkMemoryPropertyFlags hv)
        {
            if (!makeBuf(alt_.staging, kStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, true))
                return false;
            if (!makeBuf(alt_.prims, static_cast<VkDeviceSize>(maxPrims_) * kPrimStride * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true))
                return false;
            if (!makeBuf(alt_.clut, static_cast<VkDeviceSize>(maxClutSlots_) * 1024u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true))
                return false;
            if (!makeBuf(snap_, kVramSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, true, VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                return false;
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = descPool_;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &g_vk.setLayout;
            if (vkAllocateDescriptorSets(g_vk.dev, &dai, &alt_.desc) != VK_SUCCESS)
                return false;
            VkDescriptorBufferInfo bi[4] = {{vram_.b, 0, VK_WHOLE_SIZE}, {g_vk.tables, 0, VK_WHOLE_SIZE}, {alt_.clut.b, 0, VK_WHOLE_SIZE}, {alt_.prims.b, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[4]{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = alt_.desc;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bi[i];
            }
            vkUpdateDescriptorSets(g_vk.dev, 4, w, 0, nullptr);
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = g_vk.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(g_vk.dev, &cai, &alt_.cb) != VK_SUCCESS)
                return false;
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateFence(g_vk.dev, &fci, nullptr, &alt_.fence) != VK_SUCCESS)
                return false;
            presVram_.assign(kVramSize, 0u);
            return true;
        }

        // PERF-PG: PS2X_PRESENT_GPU=1 converts the display image on the GPU (gs_present.comp) and reads back only the visible rows; PS2X_PRESENT_GPU_VERIFY=1
        // does that AND the old page copy + CPU conversion and compares both frames (counted in the [presgpu] line). Needs PS2X_PRESENT_ASYNC=1.
        static bool gpuPresentWanted()
        {
            static const bool on = []() { const char *a = std::getenv("PS2X_PRESENT_GPU"); const char *b = std::getenv("PS2X_PRESENT_GPU_VERIFY"); return (a && a[0] == '1') || (b && b[0] == '1'); }();
            return on;
        }

        bool createPresentGpu()
        {
            if (!g_vk.qCompute)
            {
                std::printf("[vk] GPU present: the graphics queue family has no compute, using the CPU presenter\n");
                return false;
            }
            const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if (!makeBuf(presOut_, 16u + 640u * 512u * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true, VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                return false;
            std::memset(presOut_.map, 0, 16);
            VkDescriptorSetLayoutBinding bnd[3]{};
            for (uint32_t i = 0; i < 3; ++i)
            {
                bnd[i].binding = i;
                bnd[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bnd[i].descriptorCount = 1;
                bnd[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dsl.bindingCount = 3;
            dsl.pBindings = bnd;
            if (vkCreateDescriptorSetLayout(g_vk.dev, &dsl, nullptr, &cSetLayout_) != VK_SUCCESS)
                return false;
            VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
            VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &cSetLayout_;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(g_vk.dev, &plci, nullptr, &cPipeLayout_) != VK_SUCCESS)
                return false;
            VkShaderModule sm = makeShader(kPresentCompSpv, sizeof(kPresentCompSpv) / 4);
            if (!sm)
                return false;
            VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = sm;
            cpci.stage.pName = "main";
            cpci.layout = cPipeLayout_;
            const VkResult pr = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &cPipeline_);
            vkDestroyShaderModule(g_vk.dev, sm, nullptr);
            if (pr != VK_SUCCESS)
                return false;
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 1;
            dpi.poolSizeCount = 1;
            dpi.pPoolSizes = &ps;
            if (vkCreateDescriptorPool(g_vk.dev, &dpi, nullptr, &cPool_) != VK_SUCCESS)
                return false;
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = cPool_;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &cSetLayout_;
            if (vkAllocateDescriptorSets(g_vk.dev, &dai, &cDesc_) != VK_SUCCESS)
                return false;
            VkDescriptorBufferInfo bi[3] = {{vram_.b, 0, VK_WHOLE_SIZE}, {g_vk.tables, 0, VK_WHOLE_SIZE}, {presOut_.b, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[3]{};
            for (uint32_t i = 0; i < 3; ++i)
            {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = cDesc_;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bi[i];
            }
            vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, nullptr);
            const char *v = std::getenv("PS2X_PRESENT_GPU_VERIFY");
            gpuVerify_ = v && v[0] == '1';
            std::printf("[vk] GPU present ready (%s)\n", gpuVerify_ ? "verify: also CPU conversion, compared" : "display converted on the GPU");
            return true;
        }

        // Mirrors the decisions of GSCpuBackend::PresentFromLocalMemory for the common case: exactly one display circuit and a colour format the shader reads.
        // Anything else (both circuits blended, other formats, nothing enabled) returns false and keeps the page-copy + CPU conversion.
        static bool planGpuPresent(const GSPresentationRequest &rq, GpuPlan &p, uint32_t &why)
        {
            const auto sizeOf = [](uint64_t d, uint32_t &w, uint32_t &h)
            {
                const uint32_t dw = static_cast<uint32_t>((d >> 32) & 0x0FFFu);
                const uint32_t dh = static_cast<uint32_t>((d >> 44) & 0x07FFu);
                const uint32_t magh = static_cast<uint32_t>((d >> 23) & 0x0Fu);
                w = (dw + 1u) / (magh + 1u);
                h = dh + 1u;
                if (h > 448u && h <= 896u)
                    h = (h + 1u) / 2u;
                if (w < 64u || h < 64u)
                {
                    w = 640u;
                    h = 448u;
                }
                w = std::min<uint32_t>(w, 640u);
                h = std::min<uint32_t>(h, 512u);
            };
            const auto setup = [](uint64_t disp, uint64_t fb)
            {
                const uint32_t dw = static_cast<uint32_t>((disp >> 32) & 0x0FFFu);
                const uint32_t dh = static_cast<uint32_t>((disp >> 44) & 0x07FFu);
                const uint32_t magh = static_cast<uint32_t>((disp >> 23) & 0x0Fu);
                return ((fb >> 9) & 0x3Fu) != 0u || dw != 0u || dh != 0u || magh != 0u;
            };
            const bool valid1 = (rq.pmode & 1ull) != 0ull && setup(rq.display1, rq.dispfb1);
            const bool valid2 = (rq.pmode & 2ull) != 0ull && setup(rq.display2, rq.dispfb2);
            if (!valid1 && !valid2)
            {
                why = 0u; // none enabled
                return false;
            }
            if (valid1 && valid2)
            {
                // both circuits blended, the same arithmetic as PresentFromLocalMemory (no preferred source, no per-circuit fallback)
                const auto circuit = [](uint64_t fb, uint32_t &psm, uint32_t &base, uint32_t &fbw, uint32_t &x, uint32_t &y)
                {
                    psm = static_cast<uint32_t>((fb >> 15) & 0x1Fu);
                    base = static_cast<uint32_t>(fb & 0x1FFu) << 5u;
                    fbw = static_cast<uint32_t>((fb >> 9) & 0x3Fu);
                    if (fbw == 0u)
                        fbw = 10u;
                    x = static_cast<uint32_t>((fb >> 32) & 0x7FFu);
                    y = static_cast<uint32_t>((fb >> 43) & 0x7FFu);
                };
                circuit(rq.dispfb1, p.psm, p.base, p.fbw, p.x0, p.y0);
                circuit(rq.dispfb2, p.psm2, p.base2, p.fbw2, p.x2, p.y2);
                const auto fmtOk = [](uint32_t f) { return f == 0u || f == 1u || f == 2u || f == 10u; };
                if (!fmtOk(p.psm) || !fmtOk(p.psm2))
                {
                    why = 2u;
                    return false;
                }
                sizeOf(rq.display1, p.w, p.h);
                sizeOf(rq.display2, p.w2, p.h2);
                p.outW = std::max(p.w, p.w2);
                p.outH = std::max(p.h, p.h2);
                p.flags = 1u | ((rq.pmode & 0x80ull) ? 2u : 0u) | ((rq.pmode & 0x20ull) ? 4u : 0u) | (static_cast<uint32_t>((rq.pmode >> 8) & 0xFFu) << 8u);
                p.bg = static_cast<uint32_t>(rq.bgcolor & 0xFFFFFFull);
                p.dual = true;
                p.usedPreferred = false;
                p.displayFbp = static_cast<uint32_t>(rq.dispfb1 & 0x1FFu);
                p.sourceFbp = p.displayFbp;
                p.fieldMode = (rq.smode2 & 1ull) != 0ull && ((rq.smode2 >> 1) & 1ull) == 0ull;
                p.oddField = (rq.vsyncTick & 1ull) != 0ull;
                p.ok = true;
                return true;
            }
            const uint64_t fb = valid1 ? rq.dispfb1 : rq.dispfb2;
            const uint64_t disp = valid1 ? rq.display1 : rq.display2;
            const uint32_t dFbp = static_cast<uint32_t>(fb & 0x1FFu), dFbw = static_cast<uint32_t>((fb >> 9) & 0x3Fu), dPsm = static_cast<uint32_t>((fb >> 15) & 0x1Fu);
            sizeOf(disp, p.w, p.h);
            const bool usePref = rq.hasPreferredSource && rq.preferredDestFbp == dFbp && (rq.preferredSource.fbw != 0u || rq.preferredSource.fbp != dFbp);
            if (usePref)
            {
                p.fbw = rq.preferredSource.fbw ? rq.preferredSource.fbw : 10u;
                p.psm = rq.preferredSource.psm;
                p.base = rq.preferredSource.fbp;
                p.x0 = p.y0 = 0u;
                p.sourceFbp = rq.preferredSource.fbp;
            }
            else
            {
                p.fbw = dFbw ? dFbw : 10u;
                p.psm = dPsm;
                p.base = dFbp << 5u;
                p.x0 = static_cast<uint32_t>((fb >> 32) & 0x7FFu);
                p.y0 = static_cast<uint32_t>((fb >> 43) & 0x7FFu);
                p.sourceFbp = dFbp;
            }
            if (!(p.psm == 0u || p.psm == 1u || p.psm == 2u || p.psm == 10u))
            {
                why = 2u; // a format the shader does not read
                return false;
            }
            p.usedPreferred = usePref;
            p.displayFbp = dFbp;
            p.fieldMode = (rq.smode2 & 1ull) != 0ull && ((rq.smode2 >> 1) & 1ull) == 0ull;
            p.oddField = (rq.vsyncTick & 1ull) != 0ull;
            p.ok = true;
            return true;
        }

        // records the conversion dispatch into cb_ (outside a render pass), then makes the result visible to the host
        void recordGpuConvert(const GpuPlan &gp)
        {
            bufBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, cPipeline_);
            vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, cPipeLayout_, 0, 1, &cDesc_, 0, nullptr);
            const uint32_t ow = gp.dual ? gp.outW : gp.w, oh = gp.dual ? gp.outH : gp.h;
            const uint32_t pc[20] = {gp.base, gp.fbw, gp.psm, gp.x0, gp.y0, gp.w, gp.h, gp.flags, gp.base2, gp.fbw2,
                                     gp.psm2, gp.x2, gp.y2, gp.w2, gp.h2, gp.bg, ow, oh, 0u, 0u};
            vkCmdPushConstants(cb_, cPipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(cb_, (ow + 15u) / 16u, (oh + 15u) / 16u, 1);
            VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.buffer = presOut_.b;
            hb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
        }

        // takes the converted rows out of presOut_; needCpu = the frame is (nearly) black and the presenter would try the other context frames
        void readGpuFrame(const GpuPlan &gp, PresentationFrame &out, bool &needCpu)
        {
            needCpu = false;
            uint32_t cnt[2] = {0, 0};
            const uint32_t zero[2] = {0, 0};
            std::memcpy(cnt, presOut_.map, 8);
            std::memcpy(presOut_.map, zero, 8);
            static const bool s_fallbackOn = []() { const char *e = std::getenv("PS2X_GS_PRESENT_FALLBACK"); return e && (std::strcmp(e, "1") == 0 || std::strcmp(e, "true") == 0); }(); // FIX-BA
            if (s_fallbackOn && !gp.usedPreferred && (cnt[0] <= 256u || (gp.dual && cnt[1] <= 256u)))
            {
                needCpu = true;
                return;
            }
            const uint32_t ow = gp.dual ? gp.outW : gp.w, oh = gp.dual ? gp.outH : gp.h;
            out.pixels.assign(640u * 512u * 4u, 0u);
            const uint8_t *src = presOut_.map + 16;
            for (uint32_t y = 0; y < oh; ++y)
            {
                uint32_t sy = y;
                if (gp.fieldMode && oh >= 2u)
                {
                    sy = ((y >> 1u) << 1u) + (gp.oddField ? 1u : 0u);
                    if (sy >= oh)
                        sy = oh - 1u;
                }
                std::memcpy(out.pixels.data() + static_cast<size_t>(y) * 640u * 4u, src + static_cast<size_t>(sy) * 640u * 4u, static_cast<size_t>(ow) * 4u);
            }
            out.width = ow;
            out.height = oh;
            out.displayFbp = gp.displayFbp;
            out.sourceFbp = gp.sourceFbp;
            out.usedPreferred = gp.usedPreferred;
        }

        void swapSlot()
        {
            std::swap(staging_, alt_.staging);
            std::swap(prims_, alt_.prims);
            std::swap(clutBuf_, alt_.clut);
            std::swap(desc_, alt_.desc);
            std::swap(cb_, alt_.cb);
            std::swap(fence_, alt_.fence);
            primMap_ = reinterpret_cast<uint32_t *>(prims_.map);
        }

        // ------------------------------------------------------------ command recording
        void beginRecording()
        {
            if (recording_)
                return;
            vkResetCommandBuffer(cb_, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb_, &bi);
            vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.pipeline);
            vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.pipeLayout, 0, 1, &desc_, 0, nullptr);
            recording_ = true;
            inPass_ = false;
            if (gtQueryPool_ != VK_NULL_HANDLE && !gtInStartAsync_ && !gtCbActive_) // PROBE-GT
            {
                gtHarvestQueries();
                if ((gtCbWriteIdx_ - gtCbReadIdx_) < kGtRing)
                {
                    gtCbSlot_ = static_cast<uint32_t>(gtCbWriteIdx_ % kGtRing);
                    vkCmdResetQueryPool(cb_, gtQueryPool_, gtCbSlot_ * 2u, 2u);
                    vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gtQueryPool_, gtCbSlot_ * 2u);
                    gtCbActive_ = true;
                }
            }
            if (orderAfterAsync_)
            {
                // a submission that was not waited for (async present) may still be using vram_: order this one after it
                bufBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
                orderAfterAsync_ = false;
            }
        }

        void beginPass()
        {
            if (inPass_)
                return;
            VkViewport vp{0, 0, static_cast<float>(kViewport), static_cast<float>(kViewport), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {kViewport, kViewport}};
            vkCmdSetViewport(cb_, 0, 1, &vp);
            vkCmdSetScissor(cb_, 0, 1, &sc);
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = sc;
            ri.layerCount = 1;
            vkCmdBeginRendering(cb_, &ri);
            inPass_ = true;
        }

        // PERF-GB: consecutive prims are drawn with one instanced vkCmdDraw (the shader reads its prim record by gl_InstanceIndex, and instances rasterise
        // in order, exactly like the separate single-instance draws that were recorded back to back before)
        void flushDraws()
        {
            if (pendCount_ != 0u)
            {
                vkCmdDraw(cb_, 4, pendCount_, 0, pendFirst_);
                pendCount_ = 0;
            }
        }

        void endPass()
        {
            if (!inPass_)
                return;
            flushDraws();
            vkCmdEndRendering(cb_);
            inPass_ = false;
        }

        void bufBarrier(VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
        {
            VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.buffer = vram_.b;
            b.offset = 0;
            b.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cb_, srcStage, dstStage, 0, 0, nullptr, 1, &b, 0, nullptr);
        }

        void fragmentBarrier()
        {
            endPass();
            bufBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            unfenced_.clear();
            ++barriers_;
        }

        void resetRings()
        {
            pendCount_ = 0;
            primCount_ = 0;
            clutSlots_ = 0;
            curClutSlot_ = -1;
            stagingOff_ = 0;
            unfenced_.clear();
        }

        // contiguous runs of set pages -> (firstPage, pageCount)
        static void pageRuns(const PageSet &set, std::vector<std::pair<uint32_t, uint32_t>> &runs)
        {
            runs.clear();
            uint32_t pg = 0;
            while (pg < 512u)
            {
                if (!set.test(pg))
                {
                    ++pg;
                    continue;
                }
                uint32_t e = pg;
                while (e < 512u && set.test(e))
                    ++e;
                runs.emplace_back(pg, e - pg);
                pg = e;
            }
        }

        enum FlushReason : uint32_t
        {
            FLUSH_REASON_OTHER = 0,
            FLUSH_REASON_PRIMS = 1,
            FLUSH_REASON_CLUT = 2,
            FLUSH_REASON_STAGING = 3,
        };

        // submit everything recorded; optionally download the given GPU-newer pages into the host VRAM
        void flush(const PageSet *down, FlushReason reason = FLUSH_REASON_OTHER)
        {
            if (!recording_ && !down)
                return;
            if (!down)
            {
                switch (reason)
                {
                case FLUSH_REASON_PRIMS:   ++flushWhyPrims_; break;
                case FLUSH_REASON_CLUT:    ++flushWhyClut_; break;
                case FLUSH_REASON_STAGING: ++flushWhyStaging_; break;
                default:                   ++flushWhyOther_; break;
                }
            }
            beginRecording();
            endPass();
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            if (down)
            {
                pageRuns(*down, runs);
                std::vector<VkBufferCopy> regions;
                regions.reserve(runs.size());
                for (const auto &run : runs)
                    regions.push_back(VkBufferCopy{static_cast<VkDeviceSize>(run.first) * 8192u, static_cast<VkDeviceSize>(run.first) * 8192u,
                                                   static_cast<VkDeviceSize>(run.second) * 8192u});
                bufBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                if (!regions.empty())
                    vkCmdCopyBuffer(cb_, vram_.b, down_.b, static_cast<uint32_t>(regions.size()), regions.data());
                VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.buffer = down_.b;
                hb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
            }
            if (gtCbActive_) // PROBE-GT
            {
                vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gtQueryPool_, gtCbSlot_ * 2u + 1u);
                gtCbActive_ = false;
                ++gtCbWriteIdx_;
            }
            vkEndCommandBuffer(cb_);
            recording_ = false;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb_;
            const auto t0 = std::chrono::steady_clock::now();
            VkResult r;
            {
                GwScope gwSub(8); // PROBE-GW submit
                r = vkQueueSubmit(g_vk.queue, 1, &si, fence_);
            }
            {
                GwScope gwSy(9); // PROBE-GW sync (fence wait)
                const auto gtF0 = g_gtOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; // PROBE-GT
                if (r == VK_SUCCESS)
                    r = vkWaitForFences(g_vk.dev, 1, &fence_, VK_TRUE, ~0ull);
                if (g_gtOn) // PROBE-GT
                    g_gtFenceWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gtF0).count()), std::memory_order_relaxed);
            }
            vkResetFences(g_vk.dev, 1, &fence_);
            if (r != VK_SUCCESS)
            {
                std::printf("[vk] submit/wait failed (%d): falling back to CPU rendering\n", static_cast<int>(r));
                vkReady_ = false;
                vkFailed_ = true;
                resetRings();
                return;
            }
            if (down)
            {
                for (const auto &run : runs)
                {
                    const size_t off = static_cast<size_t>(run.first) * 8192u, len = static_cast<size_t>(run.second) * 8192u;
                    std::memcpy(host_ + off, down_.map + off, len);
                    downloadBytes_ += len;
                }
                gpuColor_.andNot(*down);
                gpuZ_.andNot(*down);
            }
            syncMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++submits_;
            resetRings();
        }

        // make the host copy of every page in `need` current (download only the GPU-newer ones among them)
        void syncPages(const PageSet &need, int reason)
        {
            if (!vkReady_)
                return;
            PageSet todo = gpuColor_;
            todo.orWith(gpuZ_);
            todo.andWith(need);
            if (!todo.any())
                return;
            ++syncs_;
            ++syncReason_[std::min(reason, 9)];
            flush(&todo);
        }

        void syncAll(int reason)
        {
            PageSet all;
            all.setAll();
            syncPages(all, reason);
        }

        void discardGpu()
        {
            if (recording_)
                flush(nullptr);
            gpuColor_.clear();
            gpuZ_.clear();
        }

        // copy host-newer pages into the GPU VRAM (inside the command stream, before the next draw)
        bool ensureUpload()
        {
            if (!hostNewer_.any())
                return true;
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            pageRuns(hostNewer_, runs);
            uint64_t total = 0;
            for (const auto &run : runs)
                total += static_cast<uint64_t>(run.second) * 8192u;
            if (stagingOff_ + total + runs.size() * 256u > kStagingSize)
                flush(nullptr, FLUSH_REASON_STAGING);
            if (!vkReady_)
                return false;
            beginRecording();
            endPass();
            const uint64_t iuT0 = g_gwOn ? __rdtsc() : 0; // PROBE-IU
            std::vector<VkBufferCopy> regions;
            regions.reserve(runs.size());
            uint32_t off = stagingOff_;
            for (const auto &run : runs)
            {
                const uint32_t addr = run.first * 8192u, len = run.second * 8192u;
                std::memcpy(staging_.map + off, host_ + addr, len);
                regions.push_back(VkBufferCopy{off, addr, len});
                off += (len + 255u) & ~255u;
            }
            vkCmdCopyBuffer(cb_, staging_.b, vram_.b, static_cast<uint32_t>(regions.size()), regions.data());
            bufBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            stagingOff_ = off;
            if (g_gwOn) ps2_iu_gpu(total, __rdtsc() - iuT0); // PROBE-IU
            uploadBytes_ += total;
            presDirty_.orWith(hostNewer_);
            hostNewer_.clear();
            return true;
        }

        // ------------------------------------------------------------ CPU path
        void cpuDraw(const GSPrimitiveBatch &batch)
        {
            if (ps2xRdTakeoverOn()) // FEAT-RD12: skip CPU fallback rasteriser
                return;
            if (vkReady_)
            {
                // the CPU draw reads/writes frame, Z and textures: simplest safe rule is a full sync
                syncAll(7);
                const auto &ctx = batch.state.context;
                const uint32_t sy1 = std::min<uint32_t>(ctx.scissor.y1, 2047u);
                const uint32_t sx1 = std::min<uint32_t>(ctx.scissor.x1, 2047u);
                pagesForRect(hostNewer_, ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, ctx.scissor.x0, sx1, ctx.scissor.y0, sy1);
                pagesForRect(hostNewer_, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, ctx.scissor.x0, sx1, ctx.scissor.y0, sy1);
            }
            ++cpuPrims_;
            cpu_->Submit(batch);
        }

        // ------------------------------------------------------------ GPU path
        void fillStateTemplate(const GSDrawState &st, int32_t clutSlot, uint32_t *tmpl) // PERF-GA
        {
            const GSContext &ctx = st.context;
            const bool sprite = (st.prim.type == GS_PRIM_SPRITE);
            std::memset(tmpl, 0, kPrimStride * 4u);
            uint32_t flags = 0;
            if (sprite) flags |= F_SPRITE;
            if (st.prim.iip) flags |= F_IIP;
            if (st.prim.tme) flags |= F_TME;
            if (st.prim.fst) flags |= F_FST;
            if (st.prim.fge) flags |= F_FGE;
            if (st.prim.abe) flags |= F_ABE;
            if (st.pabe) flags |= F_PABE;
            if (st.linearFilter) flags |= F_LINEAR;
            if (ctx.zbuf.zmask) flags |= F_ZMASK;
            tmpl[FLAGS] = flags;
            tmpl[OFX] = static_cast<uint32_t>(ctx.xyoffset.ofx >> 4);
            tmpl[OFY] = static_cast<uint32_t>(ctx.xyoffset.ofy >> 4);
            tmpl[SCX0] = ctx.scissor.x0;
            tmpl[SCX1] = ctx.scissor.x1;
            tmpl[SCY0] = ctx.scissor.y0;
            tmpl[SCY1] = ctx.scissor.y1;
            tmpl[FBP] = ctx.frame.fbp << 5u;
            tmpl[FBW] = ctx.frame.fbw;
            tmpl[FPSM] = ctx.frame.psm;
            tmpl[FBMSK] = ctx.frame.fbmsk;
            tmpl[ZBP] = ctx.zbuf.zbp << 5u;
            tmpl[ZPSM] = ctx.zbuf.psm;
            tmpl[TEST] = static_cast<uint32_t>(ctx.test);
            tmpl[ALPHA] = static_cast<uint32_t>(ctx.alpha);
            tmpl[ALPHAFIX] = static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu);
            tmpl[FBA] = static_cast<uint32_t>(ctx.fba & 1u);
            tmpl[TBP0] = ctx.tex0.tbp0;
            tmpl[TBW] = ctx.tex0.tbw;
            tmpl[TPSM] = ctx.tex0.psm;
            tmpl[TCC] = ctx.tex0.tcc;
            tmpl[TFX] = ctx.tex0.tfx;
            tmpl[CPSM] = ctx.tex0.cpsm;
            tmpl[CSM] = ctx.tex0.csm;
            tmpl[CSA] = ctx.tex0.csa;
            tmpl[TEXW] = st.textureWidth;
            tmpl[TEXH] = st.textureHeight;
            tmpl[CLAMP_LO] = static_cast<uint32_t>(ctx.clamp);
            tmpl[CLAMP_HI] = static_cast<uint32_t>(ctx.clamp >> 32);
            tmpl[TA0] = st.texa.ta0;
            tmpl[AEM] = st.texa.aem ? 1u : 0u;
            tmpl[TA1] = st.texa.ta1;
            tmpl[FOGRGB] = static_cast<uint32_t>(st.fogR) | (static_cast<uint32_t>(st.fogG) << 8) | (static_cast<uint32_t>(st.fogB) << 16);
            tmpl[CLUTSLOT] = static_cast<uint32_t>(clutSlot);
        }

        bool fillPrimFields(const GSDrawState &st, const GSVertex *verts, uint32_t vertexCount, bool sprite,
                            const uint32_t *tmpl, uint32_t *dst, int &bminX, int &bmaxX, int &bminY, int &bmaxY) // PERF-GA
        {
            const GSContext &ctx = st.context;
            const int ofx = ctx.xyoffset.ofx >> 4;
            const int ofy = ctx.xyoffset.ofy >> 4;
            alignas(64) uint32_t rec[kPrimStride];
            uint32_t *p = primStage_ ? rec : dst;

            if (sprite)
            {
                const GSVertex &v0 = verts[0];
                const GSVertex &v1 = verts[1];
                int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
                int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
                const uint32_t z1 = static_cast<uint32_t>(v1.z);
                if (x0 > x1)
                    std::swap(x0, x1);
                if (y0 > y1)
                    std::swap(y0, y1);
                const int spanX = std::max(1, x1 - x0);
                const int spanY = std::max(1, y1 - y0);
                const int ux1 = x0 + spanX - 1, uy1 = y0 + spanY - 1;
                if (ux1 < ctx.scissor.x0 || x0 > ctx.scissor.x1 || uy1 < ctx.scissor.y0 || y0 > ctx.scissor.y1)
                    return false;
                bminX = clampInt(x0, ctx.scissor.x0, ctx.scissor.x1);
                bminY = clampInt(y0, ctx.scissor.y0, ctx.scissor.y1);
                bmaxX = clampInt(ux1, ctx.scissor.x0, ctx.scissor.x1);
                bmaxY = clampInt(uy1, ctx.scissor.y0, ctx.scissor.y1);
                if (bminX > bmaxX || bminY > bmaxY)
                    return false;

                std::memcpy(p, tmpl, kPrimStride * 4u);
                p[SPR_UX0] = static_cast<uint32_t>(x0);
                p[SPR_UY0] = static_cast<uint32_t>(y0);
                p[SPR_W] = fbits(static_cast<float>(spanX));
                p[SPR_H] = fbits(static_cast<float>(spanY));
                p[SPR_Z] = z1;
                float u0f, v0f, u1f, v1f;
                if (st.prim.fst)
                {
                    u0f = static_cast<float>(v0.u >> 4);
                    v0f = static_cast<float>(v0.v >> 4);
                    u1f = static_cast<float>(v1.u >> 4);
                    v1f = static_cast<float>(v1.v >> 4);
                }
                else
                {
                    const float q0 = fabsQ(v0.q), q1 = fabsQ(v1.q);
                    u0f = (v0.s / q0) * static_cast<float>(st.textureWidth);
                    v0f = (v0.t / q0) * static_cast<float>(st.textureHeight);
                    u1f = (v1.s / q1) * static_cast<float>(st.textureWidth);
                    v1f = (v1.t / q1) * static_cast<float>(st.textureHeight);
                }
                p[SPR_U0F] = fbits(u0f);
                p[SPR_V0F] = fbits(v0f);
                p[SPR_U1F] = fbits(u1f);
                p[SPR_V1F] = fbits(v1f);
            }
            else
            {
                const GSVertex &v0 = verts[0];
                const GSVertex &v1 = verts[1];
                const GSVertex &v2 = verts[2];
                const float fx0 = v0.x - static_cast<float>(ofx), fy0 = v0.y - static_cast<float>(ofy);
                const float fx1 = v1.x - static_cast<float>(ofx), fy1 = v1.y - static_cast<float>(ofy);
                const float fx2 = v2.x - static_cast<float>(ofx), fy2 = v2.y - static_cast<float>(ofy);
                // PERF-LO: sequential compare, exactly what std::min/max(initializer_list) does (same result incl. NaN), without the list call
                float mnX = fx0, mxX = fx0, mnY = fy0, mxY = fy0;
                if (fx1 < mnX) mnX = fx1;
                if (fx2 < mnX) mnX = fx2;
                if (mxX < fx1) mxX = fx1;
                if (mxX < fx2) mxX = fx2;
                if (fy1 < mnY) mnY = fy1;
                if (fy2 < mnY) mnY = fy2;
                if (mxY < fy1) mxY = fy1;
                if (mxY < fy2) mxY = fy2;
                int minX = static_cast<int>(std::floor(mnX));
                int maxX = static_cast<int>(std::ceil(mxX));
                int minY = static_cast<int>(std::floor(mnY));
                int maxY = static_cast<int>(std::ceil(mxY));
                minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
                maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
                minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
                maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);
                const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
                if (std::fabs(denom) < 0.001f)
                    return false;
                bminX = minX;
                bmaxX = maxX;
                bminY = minY;
                bmaxY = maxY;
                if (bminX > bmaxX || bminY > bmaxY)
                    return false;

                std::memcpy(p, tmpl, kPrimStride * 4u);
                for (int i = 0; i < 3; ++i)
                {
                    const GSVertex &v = verts[i];
                    uint64_t zb;
                    std::memcpy(&zb, &v.z, 8);
                    p[Z0 + i * 2] = static_cast<uint32_t>(zb);
                    p[Z0 + i * 2 + 1] = static_cast<uint32_t>(zb >> 32);
                }
            }

            for (int i = 0; i < 3; ++i)
            {
                const GSVertex &v = verts[std::min<int>(i, std::max(0, static_cast<int>(vertexCount) - 1))];
                p[X0 + i] = fbits(v.x);
                p[Y0 + i] = fbits(v.y);
                p[Q0 + i] = fbits(v.q);
                p[S0 + i] = fbits(v.s);
                p[T0 + i] = fbits(v.t);
                p[RGBA0 + i] = static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) | (static_cast<uint32_t>(v.a) << 24);
                p[UV0 + i] = static_cast<uint32_t>(v.u) | (static_cast<uint32_t>(v.v) << 16);
                p[FOG0 + i] = v.fog;
            }
            if (sprite)
            {
                // sprite uses vertex 1 colour/fog/z: make sure those slots hold v1 even for 2-vertex batches
                const GSVertex &v1 = verts[1];
                p[RGBA0 + 1] = static_cast<uint32_t>(v1.r) | (static_cast<uint32_t>(v1.g) << 8) | (static_cast<uint32_t>(v1.b) << 16) | (static_cast<uint32_t>(v1.a) << 24);
                p[FOG0 + 1] = v1.fog;
            }
            p[BBOX + 0] = static_cast<uint32_t>(bminX);
            p[BBOX + 1] = static_cast<uint32_t>(bmaxX);
            p[BBOX + 2] = static_cast<uint32_t>(bminY);
            p[BBOX + 3] = static_cast<uint32_t>(bmaxY);

            if (primStage_)
                std::memcpy(dst, rec, kPrimStride * 4u);
            return true;
        }

        void noteRdClut(const GSDrawState &st) // FEAT-RD3c
        {
            static const bool s_on = []() { const char *e = std::getenv("PS2X_RD_VIEW"); return e && e[0] == '1'; }(); // FEAT-RD3c
            if (!s_on || !st.prim.tme || !isIndexedPsm(st.context.tex0.psm))
                return;
            RdpScopeVk rdpS; // PROBE-RDP
            const GSTex0Reg &t = st.context.tex0;
            struct Last { uint32_t tbp0, tbw, psm, tw, th, csa, cpsm; int32_t slot; };
            static thread_local Last last{~0u, 0, 0, 0, 0, 0, 0, -2};
            const Last cur{t.tbp0, t.tbw, t.psm, t.tw, t.th, t.csa, t.cpsm, curClutSlot_};
            if (std::memcmp(&last, &cur, sizeof(Last)) == 0)
                return;
            last = cur;
            ps2xRdNoteClut(t.tbp0, t.tbw, t.psm, t.tw, t.th, t.cpsm, t.csa, cpu_->m_clut.data());
        }

        int runBegin(const GSDrawState &st, uint32_t &outTlo, uint32_t &outThi, bool &outSprite, uint32_t *tmpl) // PERF-GA
        {
            const GSContext &ctx = st.context;
            const GSPrimType type = st.prim.type;
            outSprite = (type == GS_PRIM_SPRITE);
            const bool tri = (type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN);
            if (!outSprite && !tri)
                return 1;
            if (!isFramePsm(ctx.frame.psm))
                return 2;
            if (!isZPsm(ctx.zbuf.psm))
                return 3;
            if (st.prim.tme)
            {
                if (!isTexPsm(ctx.tex0.psm))
                    return 4;
                if (isIndexedPsm(ctx.tex0.psm) && !(ctx.tex0.cpsm == 0 || ctx.tex0.cpsm == 1 || ctx.tex0.cpsm == 2 || ctx.tex0.cpsm == 10))
                    return 5;
                if (st.textureWidth == 0 || st.textureHeight == 0)
                    return 5;
            }
            if (ctx.scissor.x1 > 2047 || ctx.scissor.y1 > 2047 || ctx.scissor.x0 > ctx.scissor.x1 || ctx.scissor.y0 > ctx.scissor.y1)
                return 6;
            if (maxPrimPerFrame_ && primsThisFrame_ >= maxPrimPerFrame_)
                return 7;

            if (!ensureUpload())
                return 8;
            if (clutChanged_ || curClutSlot_ < 0)
            {
                if (clutSlots_ >= maxClutSlots_)
                    flush(nullptr, FLUSH_REASON_CLUT);
                if (!vkReady_)
                    return 8;
                std::memcpy(clutBuf_.map + static_cast<size_t>(clutSlots_) * 1024u, cpu_->m_clut.data(), 1024u);
                curClutSlot_ = static_cast<int32_t>(clutSlots_++);
                clutChanged_ = false;
            }
            noteRdClut(st); // FEAT-RD3c
            if (ps2xRdTakeoverOn()) // FEAT-RD12
                return 0;
            if (!vkReady_)
                return 8;

            outTlo = 0;
            outThi = 0;
            if (st.prim.tme)
                rowsRange(ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0, std::max<uint32_t>(st.textureHeight, 1u) - 1u, outTlo, outThi);

            fillStateTemplate(st, curClutSlot_, tmpl);
            return 0;
        }

        void runEnd(const GSDrawState &st, const GSRunPrim *prims, size_t count, bool sprite, const uint32_t *tmpl,
                    uint32_t runPrims, int uMinX, int uMaxX, int uMinY, int uMaxY, uint64_t sumPrimArea,
                    uint32_t tlo, uint32_t thi, uint32_t flo, uint32_t fhi, uint32_t zlo, uint32_t zhi) // PERF-GA
        {
            const GSContext &ctx = st.context;
            beginRecording();
            if (st.prim.tme && unfenced_.overlaps(tlo, thi))
                fragmentBarrier();
            beginPass();

            const uint32_t primBase = primCount_;
            if (pendCount_ != 0u && pendFirst_ + pendCount_ == primCount_)
                pendCount_ += runPrims;
            else
            {
                flushDraws();
                pendFirst_ = primCount_;
                pendCount_ = runPrims;
            }
            primCount_ += runPrims;
            primsThisFrame_ += runPrims;
            gpuPrims_ += runPrims;

            PageSet drawPg;
            const uint64_t unionArea = static_cast<uint64_t>(uMaxX - uMinX + 1) * static_cast<uint64_t>(uMaxY - uMinY + 1);
            const bool exactUnion = (unionArea > 2u * sumPrimArea);
            if (!exactUnion)
                pagesForRect(drawPg, ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, uMinX, uMaxX, uMinY, uMaxY);
            else
                pagesForRects(drawPg, ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, runBboxes_);
            gpuColor_.orWith(drawPg);
            presDirty_.orWith(drawPg);

            PageSet runZPg;
            if (!ctx.zbuf.zmask)
            {
                if (gaCheck_)
                {
                    if (!exactUnion)
                        pagesForRect(runZPg, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, uMinX, uMaxX, uMinY, uMaxY);
                    else
                        pagesForRects(runZPg, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, runBboxes_);
                    gpuZ_.orWith(runZPg);
                }
                else
                {
                    if (!exactUnion)
                        pagesForRect(gpuZ_, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, uMinX, uMaxX, uMinY, uMaxY);
                    else
                        pagesForRects(gpuZ_, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, runBboxes_);
                }
            }
            unfenced_.add(flo, fhi);
            unfenced_.add(zlo, zhi);

            if (gaCheck_)
            {
                PageSet refColorPg, refZPg;
                uint32_t refIdx = 0;
                alignas(64) uint32_t refRec[kPrimStride];
                for (size_t i = 0; i < count; ++i)
                {
                    int bx0, bx1, by0, by1;
                    if (fillPrimFields(st, prims[i].vertices.data(),prims[i].vertexCount, sprite, tmpl, refRec, bx0, bx1, by0, by1))
                    {
                        if (refIdx < runPrims)
                        {
                            const uint32_t *runRec = primMap_ + static_cast<size_t>(primBase + refIdx) * kPrimStride;
                            if (std::memcmp(runRec, refRec, kPrimStride * 4u) != 0)
                                ++gaRecMismatch_;
                        }
                        pagesForRect(refColorPg, ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, bx0, bx1, by0, by1);
                        if (!ctx.zbuf.zmask)
                            pagesForRect(refZPg, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, bx0, bx1, by0, by1);
                        ++refIdx;
                    }
                }
                if (refIdx != runPrims)
                    ++gaRecMismatch_;

                PageSet missC = refColorPg;
                missC.andNot(drawPg);
                PageSet extraC = drawPg;
                extraC.andNot(refColorPg);
                gaPageMissing_ += popcountPageSet(missC);
                gaPageSuperset_ += popcountPageSet(extraC);

                if (!ctx.zbuf.zmask)
                {
                    PageSet missZ = refZPg;
                    missZ.andNot(runZPg);
                    PageSet extraZ = runZPg;
                    extraZ.andNot(refZPg);
                    gaPageMissing_ += popcountPageSet(missZ);
                    gaPageSuperset_ += popcountPageSet(extraZ);
                }

                ++gaRuns_;
                gaPrims_ += runPrims;
                const auto now = std::chrono::steady_clock::now();
                if (now - gaLastPrint_ >= std::chrono::seconds(5))
                {
                    std::printf("[ga] runs=%llu prims=%llu recMismatch=%llu pageSuperset=%llu pageMissing=%llu\n",
                                static_cast<unsigned long long>(gaRuns_),
                                static_cast<unsigned long long>(gaPrims_),
                                static_cast<unsigned long long>(gaRecMismatch_),
                                static_cast<unsigned long long>(gaPageSuperset_),
                                static_cast<unsigned long long>(gaPageMissing_));
                    gaLastPrint_ = now;
                }
            }
        }

        // returns 0 = drawn (or provably nothing to draw), >0 = fallback reason
        int gpuDraw(const GSPrimitiveBatch &b)
        {
            const GSDrawState &st = b.state;
            const GSContext &ctx = st.context;
            const GSPrimType type = st.prim.type;
            const bool sprite = (type == GS_PRIM_SPRITE);
            const bool tri = (type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN);
            if (!sprite && !tri)
                return 1;
            if (b.vertexCount < (sprite ? 2u : 3u))
                return 1;
            if (!isFramePsm(ctx.frame.psm))
                return 2;
            if (!isZPsm(ctx.zbuf.psm))
                return 3;
            if (st.prim.tme)
            {
                if (!isTexPsm(ctx.tex0.psm))
                    return 4;
                if (isIndexedPsm(ctx.tex0.psm) && !(ctx.tex0.cpsm == 0 || ctx.tex0.cpsm == 1 || ctx.tex0.cpsm == 2 || ctx.tex0.cpsm == 10))
                    return 5;
                if (st.textureWidth == 0 || st.textureHeight == 0)
                    return 5;
            }
            if (ctx.scissor.x1 > 2047 || ctx.scissor.y1 > 2047 || ctx.scissor.x0 > ctx.scissor.x1 || ctx.scissor.y0 > ctx.scissor.y1)
                return 6;
            if (maxPrimPerFrame_ && primsThisFrame_ >= maxPrimPerFrame_)
                return 7;

            // everything that may flush (and so reset the rings) happens before the prim block is written
            if (!ensureUpload()) // CPU-written data must be in GPU VRAM before this draw
                return 8;
            if (primCount_ >= maxPrims_)
                flush(nullptr, FLUSH_REASON_PRIMS);
            if (clutChanged_ || curClutSlot_ < 0)
            {
                if (clutSlots_ >= maxClutSlots_)
                    flush(nullptr, FLUSH_REASON_CLUT);
                if (!vkReady_)
                    return 8;
                std::memcpy(clutBuf_.map + static_cast<size_t>(clutSlots_) * 1024u, cpu_->m_clut.data(), 1024u);
                curClutSlot_ = static_cast<int32_t>(clutSlots_++);
                clutChanged_ = false;
            }
            noteRdClut(st); // FEAT-RD3c
            if (ps2xRdTakeoverOn()) // FEAT-RD12
                return 0;
            if (!vkReady_)
                return 8;

            alignas(64) uint32_t tmpl[kPrimStride];
            fillStateTemplate(st, curClutSlot_, tmpl);
            uint32_t *dst = primMap_ + static_cast<size_t>(primCount_) * kPrimStride;
            int bminX, bmaxX, bminY, bmaxY;
            if (!fillPrimFields(st, b.vertices.data(),b.vertexCount, sprite, tmpl, dst, bminX, bmaxX, bminY, bmaxY))
                return 0;
            const uint32_t flags = tmpl[FLAGS];

            // frame / Z / texture byte ranges
            uint32_t flo, fhi, zlo, zhi;
            rowsRange(ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, bminY, bmaxY, flo, fhi);
            rowsRange(ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, bminY, bmaxY, zlo, zhi);
            uint32_t tlo = 0, thi = 0;
            if (st.prim.tme)
                rowsRange(ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0, std::max<uint32_t>(st.textureHeight, 1u) - 1u, tlo, thi);

            beginRecording();
            if (st.prim.tme && unfenced_.overlaps(tlo, thi))
                fragmentBarrier();
            // a draw that reads what an earlier draw wrote as frame/Z must also be ordered; covered by the interlock for same-pixel,
            // and by the texture check above for sampled data.
            beginPass();
            if (pendCount_ != 0u && pendFirst_ + pendCount_ == primCount_)
                ++pendCount_;
            else
            {
                flushDraws();
                pendFirst_ = primCount_;
                pendCount_ = 1;
            }
            ++primCount_;
            ++primsThisFrame_;
            ++gpuPrims_;
            PageSet drawPg;
            pagesForRect(drawPg, ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, bminX, bmaxX, bminY, bmaxY);
            gpuColor_.orWith(drawPg);
            presDirty_.orWith(drawPg);
            if (compare_)
            {
                ++primSerial_;
                PageSet cp;
                pagesForRect(cp, ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, bminX, bmaxX, bminY, bmaxY);
                for (uint32_t pg = 0; pg < 512u; ++pg)
                    if (cp.test(pg))
                    {
                        PageWriter &w = lastWriter_[pg];
                        w.serial = primSerial_;
                        w.flags = flags;
                        w.fpsm = ctx.frame.psm;
                        w.tpsm = st.prim.tme ? ctx.tex0.psm : 0xFFu;
                        w.tfx = ctx.tex0.tfx;
                        w.tcc = ctx.tex0.tcc;
                        w.test = static_cast<uint32_t>(ctx.test);
                        w.alpha = static_cast<uint32_t>(ctx.alpha);
                        w.fbmsk = ctx.frame.fbmsk;
                        w.fbp = ctx.frame.fbp;
                        w.fbw = ctx.frame.fbw;
                        w.bbox[0] = bminX; w.bbox[1] = bmaxX; w.bbox[2] = bminY; w.bbox[3] = bmaxY;
                    }
            }
            if (!ctx.zbuf.zmask)
                pagesForRect(gpuZ_, ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, bminX, bmaxX, bminY, bmaxY);
            unfenced_.add(flo, fhi);
            unfenced_.add(zlo, zhi);
            return 0;
        }

        // ------------------------------------------------------------ compare / stats
        void comparePresent()
        {
            // both host copies are up to date here (GPU downloaded, CPU ops applied to both)
            if (std::memcmp(host_, refVram_.data(), kVramSize) == 0)
            {
                ++cmpClean_;
                return;
            }
            uint32_t diffWords = 0, first = ~0u, last = 0;
            const uint32_t *a = reinterpret_cast<const uint32_t *>(host_);
            const uint32_t *r = reinterpret_cast<const uint32_t *>(refVram_.data());
            uint64_t hist[4] = {0, 0, 0, 0};
            static thread_local uint32_t bigPerPage[512], bigSampleAddr[512], bigSampleGpu[512], bigSampleRef[512];
            std::memset(bigPerPage, 0, sizeof(bigPerPage));
            for (uint32_t i = 0; i < kVramSize / 4u; ++i)
                if (a[i] != r[i])
                {
                    ++diffWords;
                    if (first == ~0u)
                        first = i;
                    last = i;
                    int md = 0;
                    for (int sh = 0; sh < 32; sh += 8)
                        md = std::max(md, std::abs(static_cast<int>((a[i] >> sh) & 0xFFu) - static_cast<int>((r[i] >> sh) & 0xFFu)));
                    const int bucket = md <= 1 ? 0 : md <= 3 ? 1 : md <= 15 ? 2 : 3;
                    ++hist[bucket];
                    ++cmpHist_[bucket];
                    if (bucket == 3)
                    {
                        const uint32_t pg = (i * 4u) >> 13;
                        if (bigPerPage[pg]++ == 0)
                        {
                            bigSampleAddr[pg] = i * 4u;
                            bigSampleGpu[pg] = a[i];
                            bigSampleRef[pg] = r[i];
                        }
                    }
                }
            if (hist[3] >= 1000u && cmpBigPrinted_ < 12u)
            {
                ++cmpBigPrinted_;
                for (int rank = 0; rank < 5; ++rank)
                {
                    uint32_t best = 0, bestCount = 0;
                    for (uint32_t pg = 0; pg < 512u; ++pg)
                        if (bigPerPage[pg] > bestCount)
                        {
                            bestCount = bigPerPage[pg];
                            best = pg;
                        }
                    if (bestCount == 0)
                        break;
                    const PageWriter &w = lastWriter_[best];
                    std::printf("[vkcmp] BIG present#%u page=%u (0x%06X) words=%u sample addr=0x%06X gpu=%08X ref=%08X | last gpu writer serial=%llu flags=0x%X fpsm=%u tpsm=%u tfx=%u tcc=%u test=0x%X alpha=0x%X fbmsk=0x%X fbp=%u fbw=%u bbox=%d,%d,%d,%d\n",
                                presents_, best, best * 8192u, bestCount, bigSampleAddr[best], bigSampleGpu[best], bigSampleRef[best],
                                static_cast<unsigned long long>(w.serial), w.flags, w.fpsm, w.tpsm, w.tfx, w.tcc, w.test, w.alpha, w.fbmsk, w.fbp, w.fbw,
                                w.bbox[0], w.bbox[1], w.bbox[2], w.bbox[3]);
                    bigPerPage[best] = 0;
                }
            }
            ++cmpDirty_;
            if (cmpDirty_ <= 40u || (cmpDirty_ % 50u) == 0u)
                std::printf("[vkcmp] present#%u hist<=1/<=3/<=15/>15 = %llu/%llu/%llu/%llu (cumulative %llu/%llu/%llu/%llu)\n", presents_,
                            (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3],
                            (unsigned long long)cmpHist_[0], (unsigned long long)cmpHist_[1], (unsigned long long)cmpHist_[2], (unsigned long long)cmpHist_[3]);
            if (cmpDirty_ <= 40u || (cmpDirty_ % 50u) == 0u)
                std::printf("[vkcmp] present#%u diffwords=%u first=0x%06X last=0x%06X (gpu a=%08X ref=%08X) gpuPrims=%llu cpuPrims=%llu\n", presents_,
                            diffWords, first * 4u, last * 4u, a[first], r[first], static_cast<unsigned long long>(gpuPrims_),
                            static_cast<unsigned long long>(cpuPrims_));
            // resynchronise: GPU copy follows the reference so errors do not accumulate
            std::memcpy(host_, refVram_.data(), kVramSize);
            hostNewer_.setAll();
        }

        void maybePrintStats()
        {
            if (presents_ % 120u != 0u)
                return;
            std::printf("[vk] presents=%u gpuPrims=%llu cpuPrims=%llu syncs=%llu submits=%llu barriers=%llu sync_ms=%.1f upload_MB=%.1f fallback[line/frame/z/tex/clut/scissor/maxprim/other]=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu flushwhy[prims/clut/staging/other]=%llu/%llu/%llu/%llu%s\n",
                        presents_, static_cast<unsigned long long>(gpuPrims_), static_cast<unsigned long long>(cpuPrims_),
                        static_cast<unsigned long long>(syncs_), static_cast<unsigned long long>(submits_), static_cast<unsigned long long>(barriers_),
                        syncMs_, uploadBytes_ / 1048576.0, static_cast<unsigned long long>(fallback_[1]), static_cast<unsigned long long>(fallback_[2]),
                        static_cast<unsigned long long>(fallback_[3]), static_cast<unsigned long long>(fallback_[4]), static_cast<unsigned long long>(fallback_[5]),
                        static_cast<unsigned long long>(fallback_[6]), static_cast<unsigned long long>(fallback_[7]), static_cast<unsigned long long>(fallback_[8]),
                        static_cast<unsigned long long>(flushWhyPrims_), static_cast<unsigned long long>(flushWhyClut_),
                        static_cast<unsigned long long>(flushWhyStaging_), static_cast<unsigned long long>(flushWhyOther_),
                        compare_ ? "" : "");
            std::printf("[vk] syncs by reason present=%llu transfer=%llu clut=%llu clear=%llu writevram=%llu readvram=%llu sync=%llu cpudraw=%llu download_MB=%.1f\n",
                        static_cast<unsigned long long>(syncReason_[0]), static_cast<unsigned long long>(syncReason_[1]),
                        static_cast<unsigned long long>(syncReason_[2]), static_cast<unsigned long long>(syncReason_[3]),
                        static_cast<unsigned long long>(syncReason_[4]), static_cast<unsigned long long>(syncReason_[5]),
                        static_cast<unsigned long long>(syncReason_[6]), static_cast<unsigned long long>(syncReason_[7]), downloadBytes_ / 1048576.0);
            if (!clutDiag_.empty())
            {
                std::vector<std::pair<uint32_t, uint64_t>> v(clutDiag_.begin(), clutDiag_.end());
                std::sort(v.begin(), v.end(), [](const std::pair<uint32_t, uint64_t> &x, const std::pair<uint32_t, uint64_t> &y) { return x.second > y.second; });
                for (size_t i = 0; i < v.size() && i < 4; ++i)
                    std::printf("[vk] clut-sync page=%u (0x%06X) %s%s loads=%llu\n", v[i].first >> 2, (v[i].first >> 2) * 8192u,
                                (v[i].first & 1u) ? "colour" : "", (v[i].first & 2u) ? "+z" : "", static_cast<unsigned long long>(v[i].second));
            }
            if (compare_)
                std::printf("[vkcmp] summary clean=%llu dirty=%llu\n", static_cast<unsigned long long>(cmpClean_), static_cast<unsigned long long>(cmpDirty_));
        }

        std::mutex m_;
        std::unique_ptr<GSCpuBackend> cpu_;
        std::unique_ptr<GSCpuBackend> ref_;
        std::vector<uint8_t> refVram_;
        bool compare_ = false;
        uint32_t maxPrimPerFrame_ = 0;
        bool primStage_ = false;
        uint8_t *host_ = nullptr;
        uint32_t hostSize_ = 0;

        bool vkReady_ = false;
        bool vkFailed_ = false;
        Buf vram_, staging_, down_, prims_, clutBuf_;
        struct AltSlot
        {
            Buf staging, prims, clut;
            VkDescriptorSet desc = VK_NULL_HANDLE;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
        } alt_; // PERF-AP: the slot that is (or was last) in flight
        Buf snap_;                      // PERF-AP: display pages copied off vram_ by the async present
        std::vector<uint8_t> presVram_; // PERF-AP: private VRAM image the display conversion reads
        std::unique_ptr<GSCpuBackend> presCpu_;
        PageSet asyncPages_;
        GSPresentationRequest asyncReq_{};
        bool asyncOk_ = false, asyncPending_ = false, orderAfterAsync_ = false;
        VkDescriptorPool descPool_ = VK_NULL_HANDLE;
        VkDescriptorSet desc_ = VK_NULL_HANDLE;
        VkCommandBuffer cb_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
        uint32_t *primMap_ = nullptr;
        bool recording_ = false, inPass_ = false;

        Range unfenced_;
        PageSet gpuColor_, gpuZ_, hostNewer_;
        PageSet presDirty_; // PERF-AP: pages changed in vram_ (GPU colour writes, host uploads) since the last async present snapshot
        // PERF-PG: GPU display conversion
        Buf presOut_;
        VkDescriptorSetLayout cSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout cPipeLayout_ = VK_NULL_HANDLE;
        VkPipeline cPipeline_ = VK_NULL_HANDLE;
        VkDescriptorPool cPool_ = VK_NULL_HANDLE;
        VkDescriptorSet cDesc_ = VK_NULL_HANDLE;
        bool gpuPresOk_ = false, gpuVerify_ = false;
        int asyncKind_ = 0; // what the in-flight submission holds: bit 0 page copy, bit 1 GPU conversion
        GpuPlan gpuPlan_;
        uint64_t pgGpu_ = 0, pgPages_ = 0, pgBlack_ = 0, pgIneligible_[4] = {}, pgVerified_ = 0, pgMismatch_ = 0;
        uint64_t syncReason_[10] = {};
        std::unordered_map<uint32_t, uint64_t> clutDiag_; // (page<<2 | colour | z<<1) -> CLUT loads that forced a sync
        uint64_t cmpHist_[4] = {0, 0, 0, 0};              // differing words by max channel diff: <=1, <=3, <=15, >15
        uint32_t cmpBigPrinted_ = 0;
        struct PageWriter
        {
            uint64_t serial = 0;
            uint32_t flags = 0, fpsm = 0, tpsm = 0, tfx = 0, tcc = 0, test = 0, alpha = 0, fbmsk = 0, fbp = 0, fbw = 0;
            int bbox[4] = {0, 0, 0, 0};
        };
        PageWriter lastWriter_[512];
        uint64_t primSerial_ = 0;
        uint32_t maxPrims_ = kDefaultMaxPrims;
        uint32_t maxClutSlots_ = kDefaultMaxClutSlots;
        uint64_t flushWhyPrims_ = 0, flushWhyClut_ = 0, flushWhyStaging_ = 0, flushWhyOther_ = 0;
        uint32_t primCount_ = 0, clutSlots_ = 0, stagingOff_ = 0;
        uint32_t pendFirst_ = 0, pendCount_ = 0; // PERF-GB
        int32_t curClutSlot_ = -1;
        bool clutChanged_ = true;
        uint32_t primsThisFrame_ = 0;

        uint32_t presents_ = 0;
        uint64_t gpuPrims_ = 0, cpuPrims_ = 0, syncs_ = 0, submits_ = 0, barriers_ = 0, downloadBytes_ = 0, uploadBytes_ = 0;
        uint64_t fallback_[16] = {};
        uint64_t cmpClean_ = 0, cmpDirty_ = 0;
        double syncMs_ = 0.0;

        // PERF-GA
        std::vector<PrimBbox> runBboxes_;
        bool gaCheck_ = false;
        uint64_t gaRuns_ = 0, gaPrims_ = 0, gaRecMismatch_ = 0, gaPageSuperset_ = 0, gaPageMissing_ = 0;
        std::chrono::steady_clock::time_point gaLastPrint_;

        // PROBE-GT: GPU timestamp query pool (ring of N=8 per category, non-blocking harvest)
        static constexpr uint32_t kGtRing = 8u;
        VkQueryPool gtQueryPool_ = VK_NULL_HANDLE;
        uint64_t gtCbWriteIdx_ = 0, gtCbReadIdx_ = 0;
        uint64_t gtPresWriteIdx_ = 0, gtPresReadIdx_ = 0;
        uint32_t gtCbSlot_ = 0;
        bool gtCbActive_ = false, gtInStartAsync_ = false;

        static void gtUpdateMax(std::atomic<uint64_t> &dst, uint64_t val)
        {
            uint64_t cur = dst.load(std::memory_order_relaxed);
            while (val > cur && !dst.compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
        }

        void gtHarvestQueries()
        {
            if (gtQueryPool_ == VK_NULL_HANDLE || !g_vk.timestampOk)
                return;
            auto harvestRing = [&](uint64_t &readIdx, uint64_t writeIdx, uint32_t baseQuery,
                                   std::atomic<uint64_t> &sumNs, std::atomic<uint64_t> &maxNs, std::atomic<uint64_t> &samples)
            {
                while (readIdx < writeIdx)
                {
                    const uint32_t slot = static_cast<uint32_t>(readIdx % kGtRing);
                    uint64_t ts[2] = {};
                    const VkResult r = vkGetQueryPoolResults(
                        g_vk.dev, gtQueryPool_, baseQuery + slot * 2u, 2u,
                        sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                    if (r == VK_NOT_READY)
                        break;
                    if (r == VK_SUCCESS && ts[1] >= ts[0])
                    {
                        const uint64_t ns = static_cast<uint64_t>(static_cast<double>(ts[1] - ts[0]) * static_cast<double>(g_vk.timestampPeriod));
                        sumNs.fetch_add(ns, std::memory_order_relaxed);
                        gtUpdateMax(maxNs, ns);
                        samples.fetch_add(1, std::memory_order_relaxed);
                    }
                    ++readIdx;
                }
            };
            harvestRing(gtCbReadIdx_, gtCbWriteIdx_, 0u, g_gtFrameNs, g_gtFrameMaxNs, g_gtFrameSamples);
            harvestRing(gtPresReadIdx_, gtPresWriteIdx_, kGtRing * 2u, g_gtPresNs, g_gtPresMaxNs, g_gtPresSamples);
        }
    };
} // namespace

std::unique_ptr<GSRasterBackend> ps2x_vk_make_backend()
{
    return std::make_unique<GSVulkanBackend>();
}
