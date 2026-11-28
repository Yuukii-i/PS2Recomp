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
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

// False: no usable camera or 3D draw, the caller draws the plain composite.
bool enhBuildDebugPush(const RdEnhanceFrame &frame, int view, EnhDebugPush &push)
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
    double clipFromWorld[4][4] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                clipFromWorld[r][c] += g[r][k] * static_cast<double>(frame.camera[c * 4 + k]);

    double inv[4][4];
    if (!enhInvert4(clipFromWorld, inv))
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
void enhDrawDebug(const EnhDebugPush &push, bool useMsaa)
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
    vkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_enh.dbgPipeline);
    enhSetFullViewport();
    vkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_enh.dbgPipeLayout, 0, 1,
                            &s_enh.sets[s_vk.curFrame], 0, nullptr);
    vkCmdPushConstants(s_vk.cmd, s_enh.dbgPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
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
        enhDrawDebug(push, useMsaa);
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
