// ps2_vk_gs_backend.cpp -- Vulkan GS backend, stages V1+V2 (docs/VULKAN_PLAN.md).
//
// GSVulkanBackend wraps a GSCpuBackend. Triangle and sprite primitives with supported formats are drawn on the GPU by a
// fragment shader (vk/gs_draw.frag) that reads/writes a GPU copy of the 4 MB GS VRAM (storage buffer) with the exact PS2 swizzle.
// Everything else (transfers, CLUT loads, lines/points, presentation, unsupported formats) runs on the wrapped CPU backend,
// with the two VRAM copies synchronised by conservative byte ranges:
//   gpuWritten_ = ranges the GPU wrote that the host copy does not have yet   (download before any CPU access that overlaps)
//   hostDirty_  = ranges the CPU wrote that the GPU copy does not have yet    (staged + copied before the next GPU draw)
// Env: PS2X_GS_VULKAN=1 enables it; PS2X_GS_COMPARE=1 additionally runs a reference CPU backend and reports per-present
// VRAM mismatches ([vkcmp] lines), then resynchronises the GPU copy to the reference so errors do not accumulate.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_common.h"

// The wrapper needs the CPU backend's CLUT cache (private, and a header edit would rebuild the whole project).
// Hack Ledger: PERF-VK-1 -- `#define private public` around this single include; layout is unchanged.
#define private public
#include "runtime/gs/gs_cpu_backend.h"
#undef private

namespace GSMem
{
    const uint16_t *ExportPageTable(int idx, size_t *entries);
}

#include "vk/ps2_vk_shaders.inc"

namespace
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
    constexpr uint32_t kPrimStride = 80u; // uints per primitive, shared with the shaders
    constexpr uint32_t kMaxPrims = 24576u;
    constexpr uint32_t kMaxClutSlots = 2048u;
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
        for (uint32_t i = 0; i < qn; ++i)
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                g_vk.qf = i;
                break;
            }
        if (g_vk.qf == ~0u)
            return false;
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
            if (compare_)
                ref_ = std::make_unique<GSCpuBackend>();
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
            gpuWritten_.clear();
            hostDirty_.clear();
            hostDirty_.add(0, kVramSize);
        }

        void Reset() override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (vkReady_)
                discardGpu();
            cpu_->Reset();
            if (ref_)
                ref_->Reset();
            hostDirty_.clear();
            hostDirty_.add(0, kVramSize);
            clutChanged_ = true;
        }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->Submit(batch);
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

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->LoadClut(tex0, texclut);
            if (vkReady_)
            {
                const uint32_t lo = tex0.cbp * 256u;
                if (gpuWritten_.overlaps(lo, std::min(lo + 16384u, kVramSize)))
                    syncFromGpu();
            }
            cpu_->LoadClut(tex0, texclut);
            clutChanged_ = true;
        }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->BeginTransfer(command);
            if (vkReady_)
            {
                Range r;
                const auto &bb = command.bitbltbuf;
                const auto &tp = command.trxpos;
                const auto &tr = command.trxreg;
                const uint32_t dir = command.direction;
                if (dir == 0u || dir == 2u) // host->local, local->local: destination written
                {
                    uint32_t lo, hi;
                    rowsRange(bb.dbp, bb.dbw, bb.dpsm, tp.dsay, static_cast<uint32_t>(tp.dsay) + std::max<uint32_t>(tr.rrh, 1u) - 1u, lo, hi);
                    r.add(lo, hi);
                    if (gpuWritten_.overlaps(lo, hi))
                        syncFromGpu();
                    hostDirty_.add(lo, hi);
                }
                if (dir == 1u || dir == 2u) // local->host, local->local: source read
                {
                    uint32_t lo, hi;
                    rowsRange(bb.sbp, bb.sbw, bb.spsm, tp.ssay, static_cast<uint32_t>(tp.ssay) + std::max<uint32_t>(tr.rrh, 1u) - 1u, lo, hi);
                    if (gpuWritten_.overlaps(lo, hi))
                        syncFromGpu();
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
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->Flush();
            cpu_->Flush();
        }

        void TextureFlush() override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->TextureFlush();
            cpu_->TextureFlush();
        }

        void Sync(GSSyncReason reason) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (vkReady_)
                syncFromGpu();
            if (ref_)
                ref_->Sync(reason);
            cpu_->Sync(reason);
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (vkReady_)
                syncFromGpu();
            if (ref_)
                comparePresent();
            ++presents_;
            maybePrintStats();
            primsThisFrame_ = 0;
            return cpu_->Present(request);
        }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->ClearFramebuffer(context, rgba);
            if (vkReady_)
            {
                uint32_t lo, hi;
                rowsRange(context.frame.fbp << 5u, context.frame.fbw, context.frame.psm, 0, std::min<uint32_t>(context.scissor.y1, 2047u), lo, hi);
                if (gpuWritten_.overlaps(lo, hi))
                    syncFromGpu();
                hostDirty_.add(lo, hi);
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
                self->syncFromGpu();
            return cpu_->ReadVram(psm, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ref_)
                ref_->WriteVram(psm, base, bw, x, y, value);
            if (vkReady_)
            {
                uint32_t lo, hi;
                rowsRange(base, bw, psm, y, y, lo, hi);
                if (gpuWritten_.overlaps(lo, hi))
                    syncFromGpu();
                hostDirty_.add(lo, hi);
            }
            cpu_->WriteVram(psm, base, bw, x, y, value);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            auto *self = const_cast<GSVulkanBackend *>(this);
            std::lock_guard<std::mutex> lk(self->m_);
            if (self->vkReady_)
                self->syncFromGpu();
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
            if (!makeBuf(prims_, static_cast<VkDeviceSize>(kMaxPrims) * kPrimStride * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true))
                return false;
            if (!makeBuf(clutBuf_, static_cast<VkDeviceSize>(kMaxClutSlots) * 1024u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, true))
                return false;

            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 1;
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
            return true;
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

        void endPass()
        {
            if (!inPass_)
                return;
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
            primCount_ = 0;
            clutSlots_ = 0;
            curClutSlot_ = -1;
            stagingOff_ = 0;
            unfenced_.clear();
        }

        // submit everything recorded; optionally download the GPU-written range into the host VRAM
        void flush(bool download)
        {
            if (!recording_ && !(download && !gpuWritten_.empty()))
                return;
            beginRecording();
            endPass();
            const bool doDown = download && !gpuWritten_.empty();
            if (doDown)
            {
                bufBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferCopy c{gpuWritten_.lo, gpuWritten_.lo, gpuWritten_.hi - gpuWritten_.lo};
                vkCmdCopyBuffer(cb_, vram_.b, down_.b, 1, &c);
                VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.buffer = down_.b;
                hb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
            }
            vkEndCommandBuffer(cb_);
            recording_ = false;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb_;
            const auto t0 = std::chrono::steady_clock::now();
            VkResult r = vkQueueSubmit(g_vk.queue, 1, &si, fence_);
            if (r == VK_SUCCESS)
                r = vkWaitForFences(g_vk.dev, 1, &fence_, VK_TRUE, ~0ull);
            vkResetFences(g_vk.dev, 1, &fence_);
            if (r != VK_SUCCESS)
            {
                std::printf("[vk] submit/wait failed (%d): falling back to CPU rendering\n", static_cast<int>(r));
                vkReady_ = false;
                vkFailed_ = true;
                resetRings();
                return;
            }
            if (doDown)
            {
                std::memcpy(host_ + gpuWritten_.lo, down_.map + gpuWritten_.lo, gpuWritten_.hi - gpuWritten_.lo);
                gpuWritten_.clear();
                downloadBytes_ += 0; // (kept for stats symmetry)
            }
            syncMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++submits_;
            resetRings();
        }

        void syncFromGpu()
        {
            if (!vkReady_)
                return;
            if (gpuWritten_.empty() && !recording_)
                return;
            ++syncs_;
            flush(true);
        }

        void discardGpu()
        {
            if (recording_)
            {
                flush(false);
            }
            gpuWritten_.clear();
        }

        // copy host-dirty ranges into the GPU VRAM (inside the command stream, before the next draw)
        bool ensureUpload()
        {
            if (hostDirty_.empty())
                return true;
            const uint32_t lo = hostDirty_.lo, hi = std::min(hostDirty_.hi, kVramSize);
            const uint32_t size = hi - lo;
            if (stagingOff_ + size > kStagingSize)
                flush(false);
            if (!vkReady_)
                return false;
            beginRecording();
            endPass();
            std::memcpy(staging_.map + stagingOff_, host_ + lo, size);
            VkBufferCopy c{stagingOff_, lo, size};
            vkCmdCopyBuffer(cb_, staging_.b, vram_.b, 1, &c);
            bufBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            stagingOff_ += (size + 255u) & ~255u;
            uploadBytes_ += size;
            hostDirty_.clear();
            return true;
        }

        // ------------------------------------------------------------ CPU path
        void cpuDraw(const GSPrimitiveBatch &batch)
        {
            if (vkReady_)
            {
                // the CPU draw reads/writes frame, Z and textures: simplest safe rule is a full sync
                if (!gpuWritten_.empty() || recording_)
                    syncFromGpu();
                const auto &ctx = batch.state.context;
                uint32_t lo, hi;
                rowsRange(ctx.frame.fbp << 5u, ctx.frame.fbw, ctx.frame.psm, ctx.scissor.y0, std::min<uint32_t>(ctx.scissor.y1, 2047u), lo, hi);
                hostDirty_.add(lo, hi);
                rowsRange(ctx.zbuf.zbp << 5u, ctx.frame.fbw, ctx.zbuf.psm, ctx.scissor.y0, std::min<uint32_t>(ctx.scissor.y1, 2047u), lo, hi);
                hostDirty_.add(lo, hi);
            }
            ++cpuPrims_;
            cpu_->Submit(batch);
        }

        // ------------------------------------------------------------ GPU path
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
            if (primCount_ >= kMaxPrims)
                flush(false);
            if (clutChanged_ || curClutSlot_ < 0)
            {
                if (clutSlots_ >= kMaxClutSlots)
                    flush(false);
                if (!vkReady_)
                    return 8;
                std::memcpy(clutBuf_.map + static_cast<size_t>(clutSlots_) * 1024u, cpu_->m_clut.data(), 1024u);
                curClutSlot_ = static_cast<int32_t>(clutSlots_++);
                clutChanged_ = false;
            }
            if (!vkReady_)
                return 8;

            uint32_t *p = primMap_ + static_cast<size_t>(primCount_) * kPrimStride;
            std::memset(p, 0, kPrimStride * 4u);
            const int ofx = ctx.xyoffset.ofx >> 4;
            const int ofy = ctx.xyoffset.ofy >> 4;
            int bminX, bmaxX, bminY, bmaxY;

            if (sprite)
            {
                const GSVertex &v0 = b.vertices[0];
                const GSVertex &v1 = b.vertices[1];
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
                    return 0;
                bminX = clampInt(x0, ctx.scissor.x0, ctx.scissor.x1);
                bminY = clampInt(y0, ctx.scissor.y0, ctx.scissor.y1);
                bmaxX = clampInt(ux1, ctx.scissor.x0, ctx.scissor.x1);
                bmaxY = clampInt(uy1, ctx.scissor.y0, ctx.scissor.y1);
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
                const GSVertex &v0 = b.vertices[0];
                const GSVertex &v1 = b.vertices[1];
                const GSVertex &v2 = b.vertices[2];
                const float fx0 = v0.x - static_cast<float>(ofx), fy0 = v0.y - static_cast<float>(ofy);
                const float fx1 = v1.x - static_cast<float>(ofx), fy1 = v1.y - static_cast<float>(ofy);
                const float fx2 = v2.x - static_cast<float>(ofx), fy2 = v2.y - static_cast<float>(ofy);
                int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
                int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
                int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
                int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));
                minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
                maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
                minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
                maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);
                const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
                if (std::fabs(denom) < 0.001f)
                    return 0;
                bminX = minX;
                bmaxX = maxX;
                bminY = minY;
                bmaxY = maxY;
                for (int i = 0; i < 3; ++i)
                {
                    const GSVertex &v = b.vertices[i];
                    uint64_t zb;
                    std::memcpy(&zb, &v.z, 8);
                    p[Z0 + i * 2] = static_cast<uint32_t>(zb);
                    p[Z0 + i * 2 + 1] = static_cast<uint32_t>(zb >> 32);
                }
            }
            if (bminX > bmaxX || bminY > bmaxY)
                return 0;

            for (int i = 0; i < 3; ++i)
            {
                const GSVertex &v = b.vertices[std::min<int>(i, std::max(0, static_cast<int>(b.vertexCount) - 1))];
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
                const GSVertex &v1 = b.vertices[1];
                p[RGBA0 + 1] = static_cast<uint32_t>(v1.r) | (static_cast<uint32_t>(v1.g) << 8) | (static_cast<uint32_t>(v1.b) << 16) | (static_cast<uint32_t>(v1.a) << 24);
                p[FOG0 + 1] = v1.fog;
            }

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
            p[FLAGS] = flags;
            p[OFX] = static_cast<uint32_t>(ofx);
            p[OFY] = static_cast<uint32_t>(ofy);
            p[SCX0] = ctx.scissor.x0;
            p[SCX1] = ctx.scissor.x1;
            p[SCY0] = ctx.scissor.y0;
            p[SCY1] = ctx.scissor.y1;
            p[FBP] = ctx.frame.fbp << 5u;
            p[FBW] = ctx.frame.fbw;
            p[FPSM] = ctx.frame.psm;
            p[FBMSK] = ctx.frame.fbmsk;
            p[ZBP] = ctx.zbuf.zbp << 5u;
            p[ZPSM] = ctx.zbuf.psm;
            p[TEST] = static_cast<uint32_t>(ctx.test);
            p[ALPHA] = static_cast<uint32_t>(ctx.alpha);
            p[ALPHAFIX] = static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu);
            p[FBA] = static_cast<uint32_t>(ctx.fba & 1u);
            p[TBP0] = ctx.tex0.tbp0;
            p[TBW] = ctx.tex0.tbw;
            p[TPSM] = ctx.tex0.psm;
            p[TCC] = ctx.tex0.tcc;
            p[TFX] = ctx.tex0.tfx;
            p[CPSM] = ctx.tex0.cpsm;
            p[CSM] = ctx.tex0.csm;
            p[CSA] = ctx.tex0.csa;
            p[TEXW] = st.textureWidth;
            p[TEXH] = st.textureHeight;
            p[CLAMP_LO] = static_cast<uint32_t>(ctx.clamp);
            p[CLAMP_HI] = static_cast<uint32_t>(ctx.clamp >> 32);
            p[TA0] = st.texa.ta0;
            p[AEM] = st.texa.aem ? 1u : 0u;
            p[TA1] = st.texa.ta1;
            p[FOGRGB] = static_cast<uint32_t>(st.fogR) | (static_cast<uint32_t>(st.fogG) << 8) | (static_cast<uint32_t>(st.fogB) << 16);
            p[BBOX + 0] = static_cast<uint32_t>(bminX);
            p[BBOX + 1] = static_cast<uint32_t>(bmaxX);
            p[BBOX + 2] = static_cast<uint32_t>(bminY);
            p[BBOX + 3] = static_cast<uint32_t>(bmaxY);

            p[CLUTSLOT] = static_cast<uint32_t>(curClutSlot_);

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
            vkCmdDraw(cb_, 4, 1, 0, primCount_);
            ++primCount_;
            ++primsThisFrame_;
            ++gpuPrims_;
            gpuWritten_.add(flo, fhi);
            gpuWritten_.add(zlo, zhi);
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
            for (uint32_t i = 0; i < kVramSize / 4u; ++i)
                if (a[i] != r[i])
                {
                    ++diffWords;
                    if (first == ~0u)
                        first = i;
                    last = i;
                }
            ++cmpDirty_;
            if (cmpDirty_ <= 40u || (cmpDirty_ % 50u) == 0u)
                std::printf("[vkcmp] present#%u diffwords=%u first=0x%06X last=0x%06X (gpu a=%08X ref=%08X) gpuPrims=%llu cpuPrims=%llu\n", presents_,
                            diffWords, first * 4u, last * 4u, a[first], r[first], static_cast<unsigned long long>(gpuPrims_),
                            static_cast<unsigned long long>(cpuPrims_));
            // resynchronise: GPU copy follows the reference so errors do not accumulate
            std::memcpy(host_, refVram_.data(), kVramSize);
            hostDirty_.clear();
            hostDirty_.add(0, kVramSize);
        }

        void maybePrintStats()
        {
            if (presents_ % 120u != 0u)
                return;
            std::printf("[vk] presents=%u gpuPrims=%llu cpuPrims=%llu syncs=%llu submits=%llu barriers=%llu sync_ms=%.1f upload_MB=%.1f fallback[line/frame/z/tex/clut/scissor/maxprim/other]=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu%s\n",
                        presents_, static_cast<unsigned long long>(gpuPrims_), static_cast<unsigned long long>(cpuPrims_),
                        static_cast<unsigned long long>(syncs_), static_cast<unsigned long long>(submits_), static_cast<unsigned long long>(barriers_),
                        syncMs_, uploadBytes_ / 1048576.0, static_cast<unsigned long long>(fallback_[1]), static_cast<unsigned long long>(fallback_[2]),
                        static_cast<unsigned long long>(fallback_[3]), static_cast<unsigned long long>(fallback_[4]), static_cast<unsigned long long>(fallback_[5]),
                        static_cast<unsigned long long>(fallback_[6]), static_cast<unsigned long long>(fallback_[7]), static_cast<unsigned long long>(fallback_[8]),
                        compare_ ? "" : "");
            if (compare_)
                std::printf("[vkcmp] summary clean=%llu dirty=%llu\n", static_cast<unsigned long long>(cmpClean_), static_cast<unsigned long long>(cmpDirty_));
        }

        std::mutex m_;
        std::unique_ptr<GSCpuBackend> cpu_;
        std::unique_ptr<GSCpuBackend> ref_;
        std::vector<uint8_t> refVram_;
        bool compare_ = false;
        uint32_t maxPrimPerFrame_ = 0;
        uint8_t *host_ = nullptr;
        uint32_t hostSize_ = 0;

        bool vkReady_ = false;
        bool vkFailed_ = false;
        Buf vram_, staging_, down_, prims_, clutBuf_;
        VkDescriptorPool descPool_ = VK_NULL_HANDLE;
        VkDescriptorSet desc_ = VK_NULL_HANDLE;
        VkCommandBuffer cb_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
        uint32_t *primMap_ = nullptr;
        bool recording_ = false, inPass_ = false;

        Range gpuWritten_, hostDirty_, unfenced_;
        uint32_t primCount_ = 0, clutSlots_ = 0, stagingOff_ = 0;
        int32_t curClutSlot_ = -1;
        bool clutChanged_ = true;
        uint32_t primsThisFrame_ = 0;

        uint32_t presents_ = 0;
        uint64_t gpuPrims_ = 0, cpuPrims_ = 0, syncs_ = 0, submits_ = 0, barriers_ = 0, downloadBytes_ = 0, uploadBytes_ = 0;
        uint64_t fallback_[16] = {};
        uint64_t cmpClean_ = 0, cmpDirty_ = 0;
        double syncMs_ = 0.0;
    };
} // namespace

std::unique_ptr<GSRasterBackend> ps2x_vk_make_backend()
{
    return std::make_unique<GSVulkanBackend>();
}
