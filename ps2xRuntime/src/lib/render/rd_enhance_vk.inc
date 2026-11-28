// FEAT-ENH1: enhanced graphics post stage (Vulkan).
// Included by rd_gfx_vk.cpp inside namespace rdgfx_vk.

namespace
{

struct EnhVerifySlot // PROBE-ENH1
{
    BufferCopy before; // PROBE-ENH1
    BufferCopy after; // PROBE-ENH1
    bool pending = false; // PROBE-ENH1
    uint64_t frame = 0; // PROBE-ENH1
    uint32_t w = 0; // PROBE-ENH1
    uint32_t h = 0; // PROBE-ENH1
};

struct EnhDebugPush // FEAT-ENH2: matches shaders/fx_debug.frag
{
    float worldFromNdc[16]; // column-major
    float camRowW[4];
    float viewport[4];
    int32_t mode[4];
};
static_assert(sizeof(EnhDebugPush) <= 128, "push constants must fit in 128 bytes"); // FEAT-ENH2

struct EnhFxUniforms // FEAT-ENH4: std140, matches shaders/fx_effects.frag
{
    float worldFromNdc[16];  // column-major
    float clipFromWorld[16]; // column-major
    float camRowW[4];
    float camPos[4];   // xyz camera position, w: NDC x units per world unit at view distance 1
    float viewport[4]; // x, y, width, height of the 3D viewport
    float target[4];   // width, height, 1/width, 1/height
    float sunDir[4];   // xyz towards the sun, normalised; w: 1 if usable
    float sunColor[4];
    float ao[4];       // strength, radius, bias
    float shadow[4];   // strength, length, softness
    float fogColor[4]; // rgb, strength
    float fogRange[4]; // start, end, height, falloff
    float shafts[4];   // fog.sky, strength, length, decay
    float dof[4];      // strength, focus, range, blur
    float bloom[4];    // strength, threshold, radius
    float grade[4];    // strength, exposure, contrast, saturation
    float tint[4];
};
static_assert(sizeof(EnhFxUniforms) == (2 * 16 + 15 * 4) * sizeof(float), "std140 layout"); // FEAT-ENH4

constexpr uint32_t kEnhLevels = 4; // FEAT-ENH4: 1/2, 1/4, 1/8, 1/16

struct EnhFx // FEAT-ENH4: created on first use, destroyed with the other post targets
{
    bool ready = false;
    bool failed = false;
    VkImage level[kEnhLevels] = {};
    VkDeviceMemory levelMem[kEnhLevels] = {};
    VkImageView levelView[kEnhLevels] = {};
    uint32_t levelW[kEnhLevels] = {};
    uint32_t levelH[kEnhLevels] = {};
    std::vector<BufferCopy> ubos; // one per frame in flight
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets; // one per frame in flight
};

struct EnhState
{
    VkPipelineLayout dbgPipeLayout = VK_NULL_HANDLE; // FEAT-ENH2
    VkPipeline dbgPipeline = VK_NULL_HANDLE; // FEAT-ENH2
    bool dbgFailed = false; // FEAT-ENH2
    bool ready = false;
    bool failed = false;
    bool failLogged = false;
    uint32_t w = 0;
    uint32_t h = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage copyImage = VK_NULL_HANDLE;
    VkDeviceMemory copyMem = VK_NULL_HANDLE;
    VkImageView copyView = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets; // one per frame in flight
    std::vector<EnhVerifySlot> verify; // PROBE-ENH1
    uint64_t appliedFrames = 0; // PROBE-ENH1
    EnhFx fx; // FEAT-ENH4
};

EnhState s_enh;

bool enhVerifyOn() // PROBE-ENH1
{
    static const bool s_on = []() {
        const char *e = std::getenv("PS2X_ENH_VERIFY");
        return e && e[0] == '1';
    }();
    return s_on;
}

void enhDestroyBuffer(BufferCopy &b) // PROBE-ENH1
{
    if (b.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(s_vk.device, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE)
        vkFreeMemory(s_vk.device, b.memory, nullptr); // also unmaps
    b = BufferCopy{};
}

// FEAT-ENH4: same caller guarantee as enhanceDestroyTargets().
void enhDestroyEffects()
{
    EnhFx &fx = s_enh.fx;
    if (fx.pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(s_vk.device, fx.pipeline, nullptr);
    if (fx.pipeLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(s_vk.device, fx.pipeLayout, nullptr);
    if (fx.descPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(s_vk.device, fx.descPool, nullptr); // frees the sets
    if (fx.setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(s_vk.device, fx.setLayout, nullptr);
    for (BufferCopy &b : fx.ubos)
        enhDestroyBuffer(b);
    for (uint32_t i = 0; i < kEnhLevels; ++i)
    {
        if (fx.levelView[i] != VK_NULL_HANDLE)
            vkDestroyImageView(s_vk.device, fx.levelView[i], nullptr);
        if (fx.level[i] != VK_NULL_HANDLE)
            vkDestroyImage(s_vk.device, fx.level[i], nullptr);
        if (fx.levelMem[i] != VK_NULL_HANDLE)
            vkFreeMemory(s_vk.device, fx.levelMem[i], nullptr);
    }
    fx = EnhFx{};
}

// Caller guarantees the GPU is not using any of these (device idle, or never submitted).
void enhanceDestroyTargets()
{
    if (s_vk.device == VK_NULL_HANDLE)
        return;
    for (EnhVerifySlot &slot : s_enh.verify) // PROBE-ENH1
    {
        enhDestroyBuffer(slot.before);
        enhDestroyBuffer(slot.after);
    }
    s_enh.verify.clear(); // PROBE-ENH1
    enhDestroyEffects(); // FEAT-ENH4

    if (s_enh.dbgPipeline != VK_NULL_HANDLE) // FEAT-ENH2
        vkDestroyPipeline(s_vk.device, s_enh.dbgPipeline, nullptr);
    if (s_enh.dbgPipeLayout != VK_NULL_HANDLE) // FEAT-ENH2
        vkDestroyPipelineLayout(s_vk.device, s_enh.dbgPipeLayout, nullptr);
    s_enh.dbgPipeline = VK_NULL_HANDLE; // FEAT-ENH2
    s_enh.dbgPipeLayout = VK_NULL_HANDLE; // FEAT-ENH2
    s_enh.dbgFailed = false; // FEAT-ENH2
    if (s_enh.pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(s_vk.device, s_enh.pipeline, nullptr);
    if (s_enh.pipeLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(s_vk.device, s_enh.pipeLayout, nullptr);
    if (s_enh.descPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(s_vk.device, s_enh.descPool, nullptr);
    if (s_enh.setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(s_vk.device, s_enh.setLayout, nullptr);
    if (s_enh.copyView != VK_NULL_HANDLE)
        vkDestroyImageView(s_vk.device, s_enh.copyView, nullptr);
    if (s_enh.copyImage != VK_NULL_HANDLE)
        vkDestroyImage(s_vk.device, s_enh.copyImage, nullptr);
    if (s_enh.copyMem != VK_NULL_HANDLE)
        vkFreeMemory(s_vk.device, s_enh.copyMem, nullptr);

    s_enh.pipeline = VK_NULL_HANDLE;
    s_enh.pipeLayout = VK_NULL_HANDLE;
    s_enh.descPool = VK_NULL_HANDLE;
    s_enh.setLayout = VK_NULL_HANDLE;
    s_enh.copyView = VK_NULL_HANDLE;
    s_enh.copyImage = VK_NULL_HANDLE;
    s_enh.copyMem = VK_NULL_HANDLE;
    s_enh.sets.clear();
    s_enh.ready = false;
    s_enh.failed = false;
    s_enh.w = 0;
    s_enh.h = 0;
}

bool enhCreatePipeline(VkShaderModule vert, VkShaderModule frag, VkPipelineLayout layout,
                       VkFormat depthFmt, VkPipeline &outPipeline) // FEAT-ENH2
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = s_vk.msaaSamples;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.blendEnable = VK_FALSE;
    cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAtt;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    // Same colour format as the main pipelines (getOrCreatePipeline).
    VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rend{};
    rend.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &colorFmt;
    rend.depthAttachmentFormat = depthFmt; // FEAT-ENH2

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
    pci.layout = layout;

    return vkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &pci, nullptr, &outPipeline) == VK_SUCCESS;
}

bool enhLoadPipeline(const char *fragName, VkPipelineLayout layout, VkFormat depthFmt, VkPipeline &outPipeline)
{
    const std::filesystem::path shaderDir = getExeDir() / "shaders";
    const VkShaderModule vert = createShaderModuleFromFile(shaderDir / "fx_full.vert.spv");
    const VkShaderModule frag = createShaderModuleFromFile(shaderDir / fragName);
    const bool ok = vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE &&
                    enhCreatePipeline(vert, frag, layout, depthFmt, outPipeline);
    if (vert != VK_NULL_HANDLE)
        vkDestroyShaderModule(s_vk.device, vert, nullptr);
    if (frag != VK_NULL_HANDLE)
        vkDestroyShaderModule(s_vk.device, frag, nullptr);
    return ok;
}

bool enhCreateResources()
{
    if (!createImageAndView(s_vk.targetW, s_vk.targetH, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // FEAT-ENH4: source of the downsample chain
                            VK_IMAGE_ASPECT_COLOR_BIT, s_enh.copyImage, s_enh.copyMem, s_enh.copyView))
        return false;

    // 0: colour copy, 1: depth (read only by the debug views) // FEAT-ENH2
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s_vk.device, &dslci, nullptr, &s_enh.setLayout) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s_enh.setLayout;
    if (vkCreatePipelineLayout(s_vk.device, &plci, nullptr, &s_enh.pipeLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * s_vk.framesInFlight};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = s_vk.framesInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(s_vk.device, &dpci, nullptr, &s_enh.descPool) != VK_SUCCESS)
        return false;

    const SamplerKey sampKey{rdgfx::TextureFilter::Nearest, rdgfx::TextureFilter::Nearest,
                             rdgfx::TextureWrap::Clamp, rdgfx::TextureWrap::Clamp};
    const VkSampler sampler = getOrCreateSampler(sampKey);
    if (sampler == VK_NULL_HANDLE)
        return false;

    s_enh.sets.assign(s_vk.framesInFlight, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < s_vk.framesInFlight; ++i)
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s_enh.descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &s_enh.setLayout;
        if (vkAllocateDescriptorSets(s_vk.device, &dsai, &s_enh.sets[i]) != VK_SUCCESS)
            return false;

        VkDescriptorImageInfo info{sampler, s_enh.copyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = s_enh.sets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(s_vk.device, 1, &write, 0, nullptr);

        // FEAT-ENH2: depthView is already depth-only; it lives exactly as long as these resources
        info = {sampler, s_vk.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        write.dstBinding = 1;
        vkUpdateDescriptorSets(s_vk.device, 1, &write, 0, nullptr);
    }

    return enhLoadPipeline("fx_composite.frag.spv", s_enh.pipeLayout, VK_FORMAT_D32_SFLOAT, s_enh.pipeline);
}

bool enhEnsureResources()
{
    // Size changes also go through destroyTargets(), which drops everything here after a device idle.
    const bool same = s_enh.w == s_vk.targetW && s_enh.h == s_vk.targetH && s_enh.samples == s_vk.msaaSamples;
    if (same && (s_enh.ready || s_enh.failed))
        return s_enh.ready;

    if (s_enh.ready)
        vkDeviceWaitIdle(s_vk.device); // only on a mismatch, never per frame
    enhanceDestroyTargets();

    const bool ok = enhCreateResources();
    if (!ok)
        enhanceDestroyTargets();
    s_enh.w = s_vk.targetW;
    s_enh.h = s_vk.targetH;
    s_enh.samples = s_vk.msaaSamples;
    s_enh.ready = ok;
    s_enh.failed = !ok;

    if (ok)
    {
        ps2xSafeLog("[enh] on w=%u h=%u msaa=%d\n", s_enh.w, s_enh.h, static_cast<int>(s_enh.samples));
    }
    else if (!s_enh.failLogged)
    {
        s_enh.failLogged = true;
        ps2xSafeLog("[enh] resource creation failed, post stage disabled\n");
    }
    return ok;
}

void enhMemoryBarrier(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(s_vk.cmd, &dep);
}

// Depth (and MSAA colour) written by the previous rendering instance must be visible to the next LOAD.
void enhAttachmentBarrier()
{
    const VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    enhMemoryBarrier(stages,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     stages,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void enhEndRendering()
{
    if (!s_vk.inRenderPass)
        return;
    vkCmdEndRendering(s_vk.cmd); // resolves MSAA
    s_vk.inRenderPass = false;
    invalidateCommandBufferState();
}

void enhColorToTransferSrc(VkImage image, VkImageLayout &layout)
{
    imageBarrier(s_vk.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

void enhColorToAttachment(VkImage image, VkImageLayout &layout)
{
    imageBarrier(s_vk.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

void enhCopyToBuffer(VkImage image, VkBuffer buffer) // PROBE-ENH1
{
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {s_vk.targetW, s_vk.targetH, 1u};
    vkCmdCopyImageToBuffer(s_vk.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
}

// Returns the slot to record into for this frame, or nullptr when this frame is not verified.
EnhVerifySlot *enhVerifyAcquire() // PROBE-ENH1
{
    if (!enhVerifyOn())
        return nullptr;
    if ((++s_enh.appliedFrames % 60u) != 0u)
        return nullptr;
    if (s_enh.verify.size() != s_vk.framesInFlight)
        s_enh.verify.resize(s_vk.framesInFlight);

    EnhVerifySlot &slot = s_enh.verify[s_vk.curFrame];
    if (slot.pending)
        return nullptr; // a second apply in the same frame

    if (slot.before.buffer == VK_NULL_HANDLE || slot.after.buffer == VK_NULL_HANDLE)
    {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(s_vk.targetW) * s_vk.targetH * 4u;
        if (!createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, slot.before) ||
            !createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, slot.after))
        {
            enhDestroyBuffer(slot.before);
            enhDestroyBuffer(slot.after);
            return nullptr;
        }
    }
    slot.frame = s_enh.appliedFrames;
    slot.w = s_vk.targetW;
    slot.h = s_vk.targetH;
    return &slot;
}

// Called once the fence of frameIdx has completed, so its copies are finished.
void enhanceVerifyCollect(uint32_t frameIdx) // PROBE-ENH1
{
    if (frameIdx >= s_enh.verify.size())
        return;
    EnhVerifySlot &slot = s_enh.verify[frameIdx];
    if (!slot.pending)
        return;
    slot.pending = false;

    const uint8_t *a = static_cast<const uint8_t *>(slot.before.mapped);
    const uint8_t *b = static_cast<const uint8_t *>(slot.after.mapped);
    const size_t bytes = static_cast<size_t>(slot.w) * slot.h * 4u;
    int maxDiff = 0;
    for (size_t i = 0; i < bytes; ++i)
        maxDiff = std::max(maxDiff, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));

    ps2xSafeLog("[enh] verify frame=%llu maxdiff=%d w=%u h=%u\n",
                static_cast<unsigned long long>(slot.frame), maxDiff, slot.w, slot.h);
}

// ---- FEAT-ENH2: debug views

// Created on first use, so a missing debug shader never disables the plain composite.
bool enhEnsureDebugPipeline()
{
    if (s_enh.dbgPipeline != VK_NULL_HANDLE)
        return true;
    if (s_enh.dbgFailed)
        return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(EnhDebugPush)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s_enh.setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;

    const bool ms = s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT;
    // No depth attachment: the depth image is sampled while this pipeline draws.
    const bool ok = vkCreatePipelineLayout(s_vk.device, &plci, nullptr, &s_enh.dbgPipeLayout) == VK_SUCCESS &&
                    enhLoadPipeline(ms ? "fx_debug_ms.frag.spv" : "fx_debug.frag.spv", s_enh.dbgPipeLayout,
                                    VK_FORMAT_UNDEFINED, s_enh.dbgPipeline);
    if (!ok)
    {
        s_enh.dbgFailed = true;
        ps2xSafeLog("[enh] debug pipeline creation failed\n");
    }
    return ok;
}

bool enhInvert4(const double (&m)[4][4], double (&out)[4][4])
{
    double a[4][8] = {};
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
            a[r][c] = m[r][c];
        a[r][4 + r] = 1.0;
    }
    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col]))
                pivot = r;
        if (!(std::fabs(a[pivot][col]) > 0.0))
            return false;
        for (int c = 0; c < 8; ++c)
            std::swap(a[col][c], a[pivot][c]);
        const double inv = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c)
            a[col][c] *= inv;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col)
                continue;
            const double k = a[r][col];
            for (int c = 0; c < 8; ++c)
                a[r][c] -= k * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            out[r][c] = a[r][4 + c];
            if (!std::isfinite(out[r][c]))
                return false;
        }
    return true;
}

// World <-> NDC of this frame's world picture. False: no usable camera or 3D draw.
bool enhBuildMatrices(const RdEnhanceFrame &frame, double (&clipFromWorld)[4][4], double (&inv)[4][4]) // FEAT-ENH4
{
    if (!frame.cameraValid || s_vk.enh3dCount == 0u)
        return false;

    const double ax = s_vk.enhVpA[0], ay = s_vk.enhVpA[1], az = s_vk.enhVpA[2], aw = s_vk.enhVpA[3];
    const double bx = s_vk.enhVpB[0] / 16.0, by = s_vk.enhVpB[1] / 16.0, bz = s_vk.enhVpB[2];
    const double fx = s_vk.enhFlip[0], fy = s_vk.enhFlip[1], fz = s_vk.enhFlip[2];
    const double sx = s_vk.enhFbSize[0], sy = s_vk.enhFbSize[1];
    if (sx == 0.0 || sy == 0.0 || bz == 0.0)
        return false;

    // The linear part of shaders/rd.vert (uVpB.w > 0.5 branch, z bias, y flip, z remap), row-major.
    const double g[4][4] = {
        {2.0 * ax / sx / bz * fx, 0.0, 0.0, (2.0 * (az - bx) / sx - 1.0) / bz * fx},
        {0.0, -2.0 * ay / sy * fy, 0.0, (1.0 - 2.0 * (aw - by) / sy) * fy},
        {0.0, 0.0, 0.5 * fz, 0.5 * (1.0 - s_vk.enhZBias)},
        {0.0, 0.0, 0.0, 1.0}};

    // camera[0..3] is column 0 (VU1 quadword 998).
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            clipFromWorld[r][c] = 0.0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                clipFromWorld[r][c] += g[r][k] * static_cast<double>(frame.camera[c * 4 + k]);

    return enhInvert4(clipFromWorld, inv);
}

// False: no usable camera or 3D draw, the caller draws the plain composite.
bool enhBuildDebugPush(const RdEnhanceFrame &frame, int view, EnhDebugPush &push)
{
    double clipFromWorld[4][4];
    double inv[4][4];
    if (!enhBuildMatrices(frame, clipFromWorld, inv))
        return false;

    push = EnhDebugPush{};
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
            push.worldFromNdc[c * 4 + r] = static_cast<float>(inv[r][c]);
        push.camRowW[c] = frame.camera[c * 4 + 3];
    }
    push.viewport[0] = s_vk.enh3dViewport.x;
    push.viewport[1] = s_vk.enh3dViewport.y;
    push.viewport[2] = s_vk.enh3dViewport.width;
    push.viewport[3] = s_vk.enh3dViewport.height;
    push.mode[0] = view;
    return true;
}

void enhSetFullViewport()
{
    VkViewport vp{};
    vp.width = static_cast<float>(s_vk.targetW);
    vp.height = static_cast<float>(s_vk.targetH);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    const VkRect2D sc{{0, 0}, {s_vk.targetW, s_vk.targetH}};
    vkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    vkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
}

// Colour-only rendering instance, so depth can be sampled. Leaves no rendering active and
// returns depth to its attachment layout with the contents unchanged.
void enhDrawWithDepth(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                      const EnhDebugPush *push, bool useMsaa) // FEAT-ENH4: debug views and effects
{
    const VkPipelineStageFlags2 depthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    imageBarrier(s_vk.cmd, s_vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                 depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 s_vk.depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    s_vk.depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = s_vk.colorView;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (useMsaa)
    {
        colorAtt.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        colorAtt.resolveImageView = s_vk.resolveView;
        colorAtt.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {s_vk.targetW, s_vk.targetH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;

    vkCmdBeginRendering(s_vk.cmd, &ri);
    vkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    enhSetFullViewport();
    vkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (push)
        vkCmdPushConstants(s_vk.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*push), push);
    vkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(s_vk.cmd);

    imageBarrier(s_vk.cmd, s_vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 depthStages,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 s_vk.depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    s_vk.depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    enhAttachmentBarrier(); // colour written here is loaded by the next pass
}

// ---- FEAT-ENH4: screen-space effects

bool enhAnyEffect(const RdEnhanceSettings &s)
{
    return (s.grade.on && s.grade.strength > 0.0f) || (s.bloom.on && s.bloom.strength > 0.0f) ||
           (s.fog.on && s.fog.strength > 0.0f) || (s.ao.on && s.ao.strength > 0.0f) ||
           (s.dof.on && s.dof.strength > 0.0f) || (s.shafts.on && s.shafts.strength > 0.0f) ||
           (s.shadows.on && s.shadows.strength > 0.0f);
}

// False: no usable camera this frame.
bool enhBuildFxUniforms(const RdEnhanceFrame &frame, const RdEnhanceSettings &s, EnhFxUniforms &u)
{
    double m[4][4];
    double inv[4][4];
    if (!enhBuildMatrices(frame, m, inv))
        return false;
    if (!(s_vk.enh3dViewport.width >= 1.0f) || !(s_vk.enh3dViewport.height >= 1.0f))
        return false;

    // The camera is the point that projects to clip x = y = w = 0.
    if (!(std::fabs(inv[3][2]) > 0.0))
        return false;

    u = EnhFxUniforms{};
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            u.worldFromNdc[c * 4 + r] = static_cast<float>(inv[r][c]);
            u.clipFromWorld[c * 4 + r] = static_cast<float>(m[r][c]);
        }
        u.camRowW[c] = frame.camera[c * 4 + 3];
    }
    for (int i = 0; i < 3; ++i)
    {
        u.camPos[i] = static_cast<float>(inv[i][2] / inv[3][2]);
        if (!std::isfinite(u.camPos[i]))
            return false;
    }

    // Horizontal focal scale: row 0 without any part along the view direction (row 3).
    double dot03 = 0.0, dot33 = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        dot03 += m[0][i] * m[3][i];
        dot33 += m[3][i] * m[3][i];
    }
    if (!(dot33 > 0.0))
        return false;
    double focal2 = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double v = m[0][i] - dot03 / dot33 * m[3][i];
        focal2 += v * v;
    }
    u.camPos[3] = static_cast<float>(std::sqrt(focal2));

    u.viewport[0] = s_vk.enh3dViewport.x;
    u.viewport[1] = s_vk.enh3dViewport.y;
    u.viewport[2] = s_vk.enh3dViewport.width;
    u.viewport[3] = s_vk.enh3dViewport.height;
    u.target[0] = static_cast<float>(s_vk.targetW);
    u.target[1] = static_cast<float>(s_vk.targetH);
    u.target[2] = 1.0f / u.target[0];
    u.target[3] = 1.0f / u.target[1];

    const float sunLen = std::sqrt(s.sunDir[0] * s.sunDir[0] + s.sunDir[1] * s.sunDir[1] + s.sunDir[2] * s.sunDir[2]);
    if (sunLen > 1e-4f)
    {
        for (int i = 0; i < 3; ++i)
            u.sunDir[i] = s.sunDir[i] / sunLen;
        u.sunDir[3] = 1.0f;
    }
    for (int i = 0; i < 3; ++i)
    {
        u.sunColor[i] = s.sunColor[i];
        u.fogColor[i] = s.fog.color[i];
        u.tint[i] = s.grade.tint[i];
    }

    // A strength of 0 switches that effect off in the shader.
    u.ao[0] = s.ao.on ? s.ao.strength : 0.0f;
    u.ao[1] = s.ao.radius;
    u.ao[2] = s.ao.bias;
    u.shadow[0] = s.shadows.on ? s.shadows.strength : 0.0f;
    u.shadow[1] = s.shadows.length;
    u.shadow[2] = s.shadows.softness;
    u.fogColor[3] = s.fog.on ? s.fog.strength : 0.0f;
    u.fogRange[0] = s.fog.start;
    u.fogRange[1] = s.fog.end;
    u.fogRange[2] = s.fog.height;
    u.fogRange[3] = s.fog.falloff;
    u.shafts[0] = s.fog.sky;
    u.shafts[1] = s.shafts.on ? s.shafts.strength : 0.0f;
    u.shafts[2] = s.shafts.length;
    u.shafts[3] = s.shafts.decay;
    u.dof[0] = s.dof.on ? s.dof.strength : 0.0f;
    u.dof[1] = s.dof.focus;
    u.dof[2] = s.dof.range;
    u.dof[3] = s.dof.blur;
    u.bloom[0] = s.bloom.on ? s.bloom.strength : 0.0f;
    u.bloom[1] = s.bloom.threshold;
    u.bloom[2] = s.bloom.radius;
    u.grade[0] = s.grade.on ? s.grade.strength : 0.0f;
    u.grade[1] = s.grade.exposure;
    u.grade[2] = s.grade.contrast;
    u.grade[3] = s.grade.saturation;
    return true;
}

bool enhCreateEffects()
{
    EnhFx &fx = s_enh.fx;
    for (uint32_t i = 0; i < kEnhLevels; ++i)
    {
        fx.levelW[i] = std::max(1u, s_vk.targetW >> (i + 1));
        fx.levelH[i] = std::max(1u, s_vk.targetH >> (i + 1));
        if (!createImageAndView(fx.levelW[i], fx.levelH[i], VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, fx.level[i], fx.levelMem[i], fx.levelView[i]))
            return false;
    }

    // 0: colour copy, 1: depth, 2..5: downsampled colour, 6: settings
    constexpr uint32_t kImages = 2 + kEnhLevels;
    VkDescriptorSetLayoutBinding bindings[kImages + 1]{};
    for (uint32_t i = 0; i < kImages; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[kImages] = {kImages, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = kImages + 1;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s_vk.device, &dslci, nullptr, &fx.setLayout) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &fx.setLayout;
    if (vkCreatePipelineLayout(s_vk.device, &plci, nullptr, &fx.pipeLayout) != VK_SUCCESS)
        return false;

    const VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kImages * s_vk.framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, s_vk.framesInFlight}};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = s_vk.framesInFlight;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(s_vk.device, &dpci, nullptr, &fx.descPool) != VK_SUCCESS)
        return false;

    const VkSampler nearest = getOrCreateSampler(SamplerKey{rdgfx::TextureFilter::Nearest, rdgfx::TextureFilter::Nearest,
                                                            rdgfx::TextureWrap::Clamp, rdgfx::TextureWrap::Clamp});
    const VkSampler linear = getOrCreateSampler(SamplerKey{rdgfx::TextureFilter::Linear, rdgfx::TextureFilter::Linear,
                                                           rdgfx::TextureWrap::Clamp, rdgfx::TextureWrap::Clamp});
    if (nearest == VK_NULL_HANDLE || linear == VK_NULL_HANDLE)
        return false;

    fx.ubos.assign(s_vk.framesInFlight, BufferCopy{});
    fx.sets.assign(s_vk.framesInFlight, VK_NULL_HANDLE);
    for (uint32_t f = 0; f < s_vk.framesInFlight; ++f)
    {
        if (!createHostBuffer(sizeof(EnhFxUniforms),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, fx.ubos[f]))
            return false;

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = fx.descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &fx.setLayout;
        if (vkAllocateDescriptorSets(s_vk.device, &dsai, &fx.sets[f]) != VK_SUCCESS)
            return false;

        VkDescriptorImageInfo images[kImages];
        images[0] = {nearest, s_enh.copyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[1] = {nearest, s_vk.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        for (uint32_t i = 0; i < kEnhLevels; ++i)
            images[2 + i] = {linear, fx.levelView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo ubo{fx.ubos[f].buffer, 0, sizeof(EnhFxUniforms)};

        VkWriteDescriptorSet writes[kImages + 1]{};
        for (uint32_t i = 0; i <= kImages; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = fx.sets[f];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            if (i < kImages)
            {
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &images[i];
            }
            else
            {
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[i].pBufferInfo = &ubo;
            }
        }
        vkUpdateDescriptorSets(s_vk.device, kImages + 1, writes, 0, nullptr);
    }

    // No depth attachment: the depth image is sampled while this pipeline draws.
    const bool ms = s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT;
    return enhLoadPipeline(ms ? "fx_effects_ms.frag.spv" : "fx_effects.frag.spv", fx.pipeLayout,
                           VK_FORMAT_UNDEFINED, fx.pipeline);
}

// Created on first use, so effects that cannot be built never disable the plain composite.
bool enhEnsureEffects()
{
    if (s_enh.fx.ready)
        return true;
    if (s_enh.fx.failed)
        return false;
    if (enhCreateEffects())
    {
        s_enh.fx.ready = true;
        return true;
    }
    enhDestroyEffects(); // nothing of it has been submitted
    s_enh.fx.failed = true;
    ps2xSafeLog("[enh] effect resource creation failed\n");
    return false;
}

// Blits the colour copy down the chain. The copy starts and ends in SHADER_READ_ONLY.
void enhBuildChain()
{
    EnhFx &fx = s_enh.fx;
    // Source stage FRAGMENT chains with the barrier that made the copy readable.
    imageBarrier(s_vk.cmd, s_enh.copyImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage src = s_enh.copyImage;
    int32_t srcW = static_cast<int32_t>(s_vk.targetW);
    int32_t srcH = static_cast<int32_t>(s_vk.targetH);
    for (uint32_t i = 0; i < kEnhLevels; ++i)
    {
        // Earlier frames may still be sampling this level.
        imageBarrier(s_vk.cmd, fx.level[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        const int32_t dstW = static_cast<int32_t>(fx.levelW[i]);
        const int32_t dstH = static_cast<int32_t>(fx.levelH[i]);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {srcW, srcH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {dstW, dstH, 1};
        vkCmdBlitImage(s_vk.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       fx.level[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        imageBarrier(s_vk.cmd, fx.level[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        src = fx.level[i];
        srcW = dstW;
        srcH = dstH;
    }

    for (uint32_t i = 0; i <= kEnhLevels; ++i)
    {
        imageBarrier(s_vk.cmd, i == 0 ? s_enh.copyImage : fx.level[i - 1], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// Written in the command stream, so a second apply in the same frame keeps its own values.
void enhUploadUniforms(const EnhFxUniforms &u)
{
    enhMemoryBarrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(s_vk.cmd, s_enh.fx.ubos[s_vk.curFrame].buffer, 0, sizeof(u), &u);
    enhMemoryBarrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);
}

// True: the effects were drawn. False: nothing was recorded, the caller draws the plain composite.
bool enhApplyEffects(const RdEnhanceFrame &frame, bool useMsaa)
{
    static std::string s_loggedStage;
    const std::string stage = ps2xEnhanceStageName();
    const RdEnhanceSettings settings = ps2xEnhanceSettings();
    if (stage.empty() || !enhAnyEffect(settings))
    {
        s_loggedStage.clear();
        return false;
    }

    EnhFxUniforms u;
    if (!enhBuildFxUniforms(frame, settings, u) || !enhEnsureEffects())
        return false;

    if (s_loggedStage != stage)
    {
        s_loggedStage = stage;
        ps2xSafeLog("[enh] effects on stage=%s\n", stage.c_str());
    }

    enhBuildChain();
    enhUploadUniforms(u);
    enhDrawWithDepth(s_enh.fx.pipeline, s_enh.fx.pipeLayout, s_enh.fx.sets[s_vk.curFrame], nullptr, useMsaa);
    return true;
}

} // namespace

void enhanceApply(const RdEnhanceFrame &frame)
{
    if (!s_vk.initialized || !s_vk.frameActive || s_vk.cmd == VK_NULL_HANDLE)
        return;
    if (s_vk.firstPassInFrame)
        return; // nothing rendered yet, the colour target has no content
    if (!enhEnsureResources())
        return;

    // Pass restarts reset this; later draws must see the value they would see with the stage off.
    const int32_t savedAtInvert = s_vk.curAtInvert;

    enhEndRendering();

    const bool useMsaa = (s_vk.msaaSamples > VK_SAMPLE_COUNT_1_BIT && s_vk.resolveImage != VK_NULL_HANDLE);
    const VkImage srcImage = useMsaa ? s_vk.resolveImage : s_vk.colorImage;
    VkImageLayout &srcLayout = useMsaa ? s_vk.resolveLayout : s_vk.colorLayout;

    enhColorToTransferSrc(srcImage, srcLayout);

    // The previous frame may still be sampling the copy image: wait for its fragment stage.
    imageBarrier(s_vk.cmd, s_enh.copyImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {s_vk.targetW, s_vk.targetH, 1u};
    vkCmdCopyImage(s_vk.cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s_enh.copyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    imageBarrier(s_vk.cmd, s_enh.copyImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    EnhVerifySlot *verify = enhVerifyAcquire(); // PROBE-ENH1
    if (verify) // PROBE-ENH1
        enhCopyToBuffer(srcImage, verify->before.buffer);

    enhColorToAttachment(srcImage, srcLayout);
    enhAttachmentBarrier();

    EnhDebugPush push; // FEAT-ENH2
    const int view = ps2xEnhanceDebugView(); // FEAT-ENH2
    if (view != 0 && enhBuildDebugPush(frame, view, push) && enhEnsureDebugPipeline()) // FEAT-ENH2
    {
        enhDrawWithDepth(s_enh.dbgPipeline, s_enh.dbgPipeLayout, s_enh.sets[s_vk.curFrame], &push, useMsaa);
    }
    else if (view == 0 && enhApplyEffects(frame, useMsaa)) // FEAT-ENH4
    {
    }
    else
    {
        // firstPassInFrame is false here, so colour and depth are LOADed and STOREd.
        ensureRenderPass(false, 0.0f, 0.0f, 0.0f, 1.0f);

        vkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_enh.pipeline);
        enhSetFullViewport();
        vkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_enh.pipeLayout, 0, 1,
                                &s_enh.sets[s_vk.curFrame], 0, nullptr);
        vkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    }

    if (verify) // PROBE-ENH1
    {
        enhEndRendering(); // resolve again
        enhColorToTransferSrc(srcImage, srcLayout);
        enhCopyToBuffer(srcImage, verify->after.buffer);
        enhColorToAttachment(srcImage, srcLayout);
        enhMemoryBarrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        verify->pending = true;
        enhAttachmentBarrier();
        ensureRenderPass(false, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    // Our binds replaced pipeline, descriptor set, viewport and scissor: force a rebind.
    invalidateCommandBufferState();
    s_vk.curAtInvert = savedAtInvert;
}
