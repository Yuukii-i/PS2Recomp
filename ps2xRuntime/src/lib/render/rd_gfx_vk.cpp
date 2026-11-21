// RDG2: Vulkan implementation of rdgfx:: interface.

#include "rd_gfx_vk.h"

#if defined(PS2X_HAS_SDL3)

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
};
static_assert(sizeof(PushConstants) <= 128, "PushConstants must fit in 128 bytes");

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
};

struct GarbageBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct GarbageTexture
{
    VkImageView view = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
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

    std::vector<VkDescriptorPool> descPools;
    std::unordered_map<uint32_t, VkSampler> samplers;

    // Offscreen render targets
    uint32_t targetW = 0;
    uint32_t targetH = 0;
    VkImage colorImage = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;

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

    // Resource tables
    uint32_t nextBufId = 1;
    std::unordered_map<uint32_t, GpuBuffer> vbos;
    std::unordered_map<uint32_t, GpuBuffer> ebos;

    uint32_t nextTexId = 1;
    std::unordered_map<uint32_t, GpuTexture> textures;
    uint32_t defaultWhiteTex = 0;

    std::vector<std::vector<GarbageBuffer>> garbageBuffers;
    std::vector<std::vector<GarbageTexture>> garbageTextures;

    // Debug stats
    bool logEnabled = false;
    uint64_t statFrames = 0;
    uint64_t statDraws = 0;
    uint64_t totalBufBytes = 0;
    std::chrono::steady_clock::time_point lastLogTime{};
};

RdVkState s_vk;

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
    vkQueueWaitIdle(s_vk.queue);
    vkFreeCommandBuffers(s_vk.device, s_vk.uploadPool, 1, &cmd);
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

void destroyTargets()
{
    if (s_vk.device == VK_NULL_HANDLE)
        return;
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
    s_vk.colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.targetW = 0;
    s_vk.targetH = 0;
}

bool createImageAndView(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, VkImage &outImg, VkDeviceMemory &outMem, VkImageView &outView)
{
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
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

    vkDeviceWaitIdle(s_vk.device);
    destroyTargets();

    if (!createImageAndView(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, s_vk.colorImage, s_vk.colorMem, s_vk.colorView))
    {
        destroyTargets();
        return false;
    }

    if (!createImageAndView(w, h, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, s_vk.depthImage, s_vk.depthMem, s_vk.depthView))
    {
        destroyTargets();
        return false;
    }

    s_vk.targetW = w;
    s_vk.targetH = h;
    s_vk.colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_vk.depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

void applyViewportAndScissor()
{
    if (!s_vk.frameActive)
        return;
    const int th = static_cast<int>(s_vk.targetH);
    VkViewport vp{};
    vp.x = static_cast<float>(s_vk.vpX);
    vp.y = static_cast<float>(th - (s_vk.vpY + s_vk.vpH));
    vp.width = static_cast<float>(s_vk.vpW);
    vp.height = static_cast<float>(s_vk.vpH);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(s_vk.cmd, 0, 1, &vp);

    VkRect2D sc = computeVkScissor();
    vkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
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
    s_vk.inRenderPass = true;
    s_vk.firstPassInFrame = false;
    s_vk.boundPipeline = VK_NULL_HANDLE;
    applyViewportAndScissor();
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

VkPipeline getOrCreatePipeline(const PipelineKey &key)
{
    const uint64_t packed = key.pack();
    auto it = s_vk.pipelines.find(packed);
    if (it != s_vk.pipelines.end())
        return it->second;

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
    binding.stride = 28;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 16};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 20};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    if (vkAllocateMemory(s_vk.device, &mai, nullptr, &out.memory) != VK_SUCCESS)
        return false;
    if (vkBindBufferMemory(s_vk.device, out.buffer, out.memory, 0) != VK_SUCCESS)
        return false;
    if (vkMapMemory(s_vk.device, out.memory, 0, size, 0, &out.mapped) != VK_SUCCESS)
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
    const uint32_t id = s_vk.nextBufId++;
    if (isIndex)
        s_vk.ebos.emplace(id, std::move(buf));
    else
        s_vk.vbos.emplace(id, std::move(buf));
    return id;
}

void updateDynBuffer(std::unordered_map<uint32_t, GpuBuffer> &map, uint32_t id, const void *data, int sizeBytes, int offsetBytes)
{
    if (!data || sizeBytes <= 0 || offsetBytes < 0)
        return;
    auto it = map.find(id);
    if (it == map.end())
        return;
    GpuBuffer &buf = it->second;
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

void freeDynBuffer(std::unordered_map<uint32_t, GpuBuffer> &map, uint32_t id)
{
    auto it = map.find(id);
    if (it == map.end())
        return;
    GpuBuffer &buf = it->second;
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
    map.erase(it);
}

void flushGarbage(uint32_t frameIdx)
{
    if (frameIdx >= s_vk.framesInFlight)
        return;
    for (const auto &gb : s_vk.garbageBuffers[frameIdx])
    {
        if (gb.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(s_vk.device, gb.buffer, nullptr);
        if (gb.memory != VK_NULL_HANDLE)
            vkFreeMemory(s_vk.device, gb.memory, nullptr);
    }
    s_vk.garbageBuffers[frameIdx].clear();

    for (const auto &gt : s_vk.garbageTextures[frameIdx])
    {
        if (gt.view != VK_NULL_HANDLE)
            vkDestroyImageView(s_vk.device, gt.view, nullptr);
        if (gt.image != VK_NULL_HANDLE)
            vkDestroyImage(s_vk.device, gt.image, nullptr);
        if (gt.memory != VK_NULL_HANDLE)
            vkFreeMemory(s_vk.device, gt.memory, nullptr);
    }
    s_vk.garbageTextures[frameIdx].clear();
}

void applyPendingBufferWrites(uint32_t frameIdx)
{
    auto applyMap = [frameIdx](std::unordered_map<uint32_t, GpuBuffer> &map)
    {
        for (auto &[id, buf] : map)
        {
            (void)id;
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
    applyMap(s_vk.vbos);
    applyMap(s_vk.ebos);
}

bool prepareDraw()
{
    if (!s_vk.frameActive || !s_vk.shaderReady)
        return false;

    ensureRenderPass(false, 0.0f, 0.0f, 0.0f, 1.0f);

    VkPipeline pipe = getOrCreatePipeline(s_vk.pipeKey);
    if (pipe == VK_NULL_HANDLE)
        return false;
    if (pipe != s_vk.boundPipeline)
    {
        vkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        s_vk.boundPipeline = pipe;
    }

    applyViewportAndScissor();

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s_vk.descPools[s_vk.curFrame];
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &s_vk.descSetLayout;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(s_vk.device, &dsai, &ds) != VK_SUCCESS)
        return false;

    auto resolveTex = [](uint32_t id) -> const GpuTexture *
    {
        auto it = s_vk.textures.find(id);
        if (it != s_vk.textures.end())
            return &it->second;
        auto defIt = s_vk.textures.find(s_vk.defaultWhiteTex);
        return (defIt != s_vk.textures.end()) ? &defIt->second : nullptr;
    };

    const GpuTexture *tex0 = resolveTex(s_vk.boundTex[0]);
    const GpuTexture *tex1 = resolveTex(s_vk.boundTex[1]);
    if (!tex0 || !tex1)
        return false;

    VkDescriptorImageInfo imgInfos[2]{};
    imgInfos[0] = {getOrCreateSampler(tex0->sampler), tex0->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgInfos[1] = {getOrCreateSampler(tex1->sampler), tex1->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

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
    vkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipelineLayout, 0, 1, &ds, 0, nullptr);

    vkCmdPushConstants(s_vk.cmd, s_vk.pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &s_vk.pc);

    auto vboIt = s_vk.vbos.find(s_vk.boundVbo);
    if (vboIt == s_vk.vbos.end())
        return false;
    VkBuffer vb = vboIt->second.copies[s_vk.curFrame].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vb, &offset);

    ++s_vk.statDraws;
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

    s_vk.descPools.resize(s_vk.framesInFlight, VK_NULL_HANDLE);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4096;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    for (uint32_t i = 0; i < s_vk.framesInFlight; ++i)
    {
        if (vkCreateDescriptorPool(s_vk.device, &dpci, nullptr, &s_vk.descPools[i]) != VK_SUCCESS)
            return false;
    }

    s_vk.garbageBuffers.resize(s_vk.framesInFlight);
    s_vk.garbageTextures.resize(s_vk.framesInFlight);

    s_vk.pc = PushConstants{};
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

    s_vk.cmd = cmd;
    s_vk.swapImage = swapImg;
    s_vk.curFrame = frameIdx;
    s_vk.inRenderPass = false;
    s_vk.firstPassInFrame = true;
    s_vk.boundPipeline = VK_NULL_HANDLE;

    if (!ensureTargets(w, h))
    {
        s_vk.frameActive = false;
        return;
    }

    vkResetDescriptorPool(s_vk.device, s_vk.descPools[frameIdx], 0);
    flushGarbage(frameIdx);
    applyPendingBufferWrites(frameIdx);

    s_vk.frameActive = true;
}

void endFrame()
{
    if (!s_vk.initialized || !s_vk.frameActive)
        return;

    if (s_vk.inRenderPass)
    {
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
    }
    else if (s_vk.firstPassInFrame)
    {
        ensureRenderPass(true, 0.0f, 0.0f, 0.0f, 1.0f);
        vkCmdEndRendering(s_vk.cmd);
        s_vk.inRenderPass = false;
    }

    imageBarrier(s_vk.cmd, s_vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 s_vk.colorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    s_vk.colorLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    imageBarrier(s_vk.cmd, s_vk.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(s_vk.targetW), static_cast<int32_t>(s_vk.targetH), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(s_vk.targetW), static_cast<int32_t>(s_vk.targetH), 1};
    vkCmdBlitImage(s_vk.cmd,
                   s_vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s_vk.swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    imageBarrier(s_vk.cmd, s_vk.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    ps2xSdlVkEndRdFrame();
    s_vk.frameActive = false;

    ++s_vk.statFrames;
    if (s_vk.logEnabled)
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - s_vk.lastLogTime).count() >= 1)
        {
            double bufMB = static_cast<double>(s_vk.totalBufBytes) / (1024.0 * 1024.0);
            std::printf("[rdvk] frames=%llu draws=%llu pipelines=%zu bufMB=%.2f\n",
                        static_cast<unsigned long long>(s_vk.statFrames),
                        static_cast<unsigned long long>(s_vk.statDraws),
                        s_vk.pipelines.size(),
                        bufMB);
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
    if (u == rdgfx::Uniform::VpA)
        std::memcpy(s_vk.pc.uVpA, v, 4 * sizeof(float));
    else if (u == rdgfx::Uniform::VpB)
        std::memcpy(s_vk.pc.uVpB, v, 4 * sizeof(float));
}

void flushBatch() {}

void getBackbufferSize(int &rw, int &rh)
{
    rw = static_cast<int>(s_vk.targetW);
    rh = static_cast<int>(s_vk.targetH);
}

void setViewport(int x, int y, int w, int h)
{
    s_vk.vpX = x;
    s_vk.vpY = y;
    s_vk.vpW = w;
    s_vk.vpH = h;
    if (s_vk.inRenderPass)
        applyViewportAndScissor();
}

void setScissorTest(bool on)
{
    s_vk.scissorTest = on;
    if (s_vk.inRenderPass)
        applyViewportAndScissor();
}

void setScissorRect(int x, int y, int w, int h)
{
    s_vk.scX = x;
    s_vk.scY = y;
    s_vk.scW = w;
    s_vk.scH = h;
    if (s_vk.inRenderPass)
        applyViewportAndScissor();
}

void setColorMask(bool r, bool g, bool b, bool a)
{
    uint8_t m = 0;
    if (r) m |= VK_COLOR_COMPONENT_R_BIT;
    if (g) m |= VK_COLOR_COMPONENT_G_BIT;
    if (b) m |= VK_COLOR_COMPONENT_B_BIT;
    if (a) m |= VK_COLOR_COMPONENT_A_BIT;
    s_vk.pipeKey.colorMask = m;
}

void setDepthTest(bool on) { s_vk.pipeKey.depthTest = on; }
void setDepthMask(bool on) { s_vk.pipeKey.depthMask = on; }
void setDepthFunc(rdgfx::DepthFunc func) { s_vk.pipeKey.depthFunc = func; }

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
    s_vk.pipeKey.blendEnable = enable;
    s_vk.pipeKey.srcRgb = srcRgb;
    s_vk.pipeKey.dstRgb = dstRgb;
    s_vk.pipeKey.srcAlpha = srcAlpha;
    s_vk.pipeKey.dstAlpha = dstAlpha;
    s_vk.pipeKey.eqRgb = eqRgb;
    s_vk.pipeKey.eqAlpha = eqAlpha;
}

void setBackfaceCulling(bool on) { s_vk.pipeKey.cull = on; }

void clearScreenBuffers(float r, float g, float b, float a)
{
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

uint32_t createVertexArray() { return 1u; }
void bindVertexArray(uint32_t) {}

uint32_t createVertexBuffer(const void *data, int sizeBytes, bool)
{
    return createDynBuffer(data, sizeBytes, false);
}

void updateVertexBuffer(uint32_t vbo, const void *data, int sizeBytes, int offsetBytes)
{
    updateDynBuffer(s_vk.vbos, vbo, data, sizeBytes, offsetBytes);
}

void freeVertexBuffer(uint32_t vbo)
{
    freeDynBuffer(s_vk.vbos, vbo);
}

uint32_t createIndexBuffer(const void *data, int sizeBytes, bool)
{
    return createDynBuffer(data, sizeBytes, true);
}

void updateIndexBuffer(uint32_t ebo, const void *data, int sizeBytes, int offsetBytes)
{
    updateDynBuffer(s_vk.ebos, ebo, data, sizeBytes, offsetBytes);
}

void freeIndexBuffer(uint32_t ebo)
{
    freeDynBuffer(s_vk.ebos, ebo);
}

void bindVertexBuffer(uint32_t vbo) { s_vk.boundVbo = vbo; }
void bindIndexBuffer(uint32_t ebo) { s_vk.boundEbo = ebo; }
void setVertexAttribute(uint32_t, int, rdgfx::VertexAttrFormat, bool, int, int) {}
void enableVertexAttribute(uint32_t) {}

void drawArrays(rdgfx::PrimitiveTopology, int first, int count)
{
    if (count <= 0 || !prepareDraw())
        return;
    vkCmdDraw(s_vk.cmd, static_cast<uint32_t>(count), 1, static_cast<uint32_t>(first), 0);
}

void drawIndexed(rdgfx::PrimitiveTopology, uint32_t first, uint32_t count, bool idx32)
{
    if (count == 0 || !prepareDraw())
        return;
    auto eboIt = s_vk.ebos.find(s_vk.boundEbo);
    if (eboIt == s_vk.ebos.end())
        return;
    VkBuffer eb = eboIt->second.copies[s_vk.curFrame].buffer;
    vkCmdBindIndexBuffer(s_vk.cmd, eb, 0, idx32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(s_vk.cmd, count, 1, first, 0, 0);
}

uint32_t createTextureRGBA8(const void *data, int w, int h)
{
    if (!s_vk.initialized || w <= 0 || h <= 0)
        return 0;

    const uint32_t maxDim = static_cast<uint32_t>(std::max(w, h));
    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) + 1u;

    GpuTexture tex;
    tex.width = w;
    tex.height = h;
    tex.mipLevels = mipLevels;

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

    BufferCopy staging{};
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(w) * h * 4;
    if (data)
    {
        if (!createHostBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging))
        {
            vkDestroyImageView(s_vk.device, tex.view, nullptr);
            vkFreeMemory(s_vk.device, tex.memory, nullptr);
            vkDestroyImage(s_vk.device, tex.image, nullptr);
            return 0;
        }
        std::memcpy(staging.mapped, data, static_cast<size_t>(byteSize));
    }

    VkCommandBuffer cmd = beginOneTimeCmd();
    if (data)
    {
        imageBarrier(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
    endOneTimeCmd(cmd);

    if (data)
    {
        vkUnmapMemory(s_vk.device, staging.memory);
        vkDestroyBuffer(s_vk.device, staging.buffer, nullptr);
        vkFreeMemory(s_vk.device, staging.memory, nullptr);
    }

    const uint32_t id = s_vk.nextTexId++;
    s_vk.textures.emplace(id, tex);
    return id;
}

void freeTexture(uint32_t texId)
{
    auto it = s_vk.textures.find(texId);
    if (it == s_vk.textures.end())
        return;
    const GpuTexture &tex = it->second;
    if (s_vk.frameActive)
        s_vk.garbageTextures[s_vk.curFrame].push_back({tex.view, tex.image, tex.memory});
    else
    {
        if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(s_vk.device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE) vkDestroyImage(s_vk.device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(s_vk.device, tex.memory, nullptr);
    }
    s_vk.textures.erase(it);
}

void generateMipmaps(uint32_t texId, int w, int h)
{
    auto it = s_vk.textures.find(texId);
    if (it == s_vk.textures.end())
        return;
    GpuTexture &tex = it->second;
    if (tex.mipLevels <= 1)
        return;

    VkCommandBuffer cmd = beginOneTimeCmd();
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

    endOneTimeCmd(cmd);
}

void setTextureWrap(uint32_t texId, bool isT, rdgfx::TextureWrap wrap)
{
    auto it = s_vk.textures.find(texId);
    if (it == s_vk.textures.end())
        return;
    if (isT)
        it->second.sampler.wrapT = wrap;
    else
        it->second.sampler.wrapS = wrap;
}

void setTextureFilter(uint32_t texId, bool isMin, rdgfx::TextureFilter filter)
{
    auto it = s_vk.textures.find(texId);
    if (it == s_vk.textures.end())
        return;
    if (isMin)
        it->second.sampler.minFilter = filter;
    else
        it->second.sampler.magFilter = filter;
}

void bindTexture(uint32_t unit, uint32_t texId)
{
    if (unit < 2)
        s_vk.boundTex[unit] = texId;
}

// Not in RDG2A (palette textures / overlay / backbuffer copy deferred to later tasks).
bool isPaletteTextureSupported() { return false; }
bool uploadPaletteTexture(const uint32_t *) { return false; }
void unbindPaletteTexture() {}

void drawTextureOverlay(uint32_t, int, int, float, float, float, float, float, float, float, float, uint8_t) {}

bool canCopyBackbuffer() { return false; }
bool beginBackbufferCopy(rdgfx::BackbufferCopyState &) { return false; }
bool copyBackbufferToTexture(rdgfx::BackbufferCopyState &, uint32_t &, uint32_t &, int &, int &,
                             int, int, int, int, int, int, int, int, bool)
{
    return false;
}
void endBackbufferCopy(const rdgfx::BackbufferCopyState &) {}

uint32_t getLastError() { return 0u; }

} // namespace rdgfx_vk

#endif // PS2X_HAS_SDL3
