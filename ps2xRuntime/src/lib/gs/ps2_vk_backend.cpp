// ps2_vk_backend.cpp -- Vulkan GS backend, stage V0 (bootstrap self-test). See docs/VULKAN_PLAN.md.
// Creates an instance/device, clears an offscreen RGBA8 image, reads it back, reports fragment-shader-interlock support.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    bool vkOk(VkResult r, const char *what)
    {
        if (r != VK_SUCCESS)
        {
            std::printf("[vk] FAIL %s -> %d\n", what, static_cast<int>(r));
            return false;
        }
        return true;
    }

    uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags want)
    {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        }
        return ~0u;
    }
}

extern "C" bool ps2x_vk_selftest()
{
    constexpr uint32_t kW = 640u, kH = 512u;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ps2x";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (!vkOk(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance"))
        return false;

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, nullptr);
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(inst, &pdCount, pds.data());
    if (pds.empty())
    {
        std::printf("[vk] no physical devices\n");
        return false;
    }

    // Prefer a discrete GPU.
    VkPhysicalDevice pd = pds[0];
    for (VkPhysicalDevice cand : pds)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(cand, &p);
        std::printf("[vk] device: %s type=%d api=%u.%u.%u\n", p.deviceName, static_cast<int>(p.deviceType),
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            pd = cand;
            break;
        }
    }

    // Report fragment shader interlock + int64/storage features (needed by stage V2).
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
    bool hasInterlock = false;
    for (const auto &e : exts)
        if (std::strcmp(e.extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0)
            hasInterlock = true;
    std::printf("[vk] VK_EXT_fragment_shader_interlock: %s\n", hasInterlock ? "yes" : "NO");

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
    uint32_t qf = ~0u;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            qf = i;
            break;
        }
    if (qf == ~0u)
    {
        std::printf("[vk] no graphics queue\n");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    if (!vkOk(vkCreateDevice(pd, &dci, nullptr, &dev), "vkCreateDevice"))
        return false;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    // Offscreen image.
    VkImageCreateInfo imgci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgci.extent = {kW, kH, 1};
    imgci.mipLevels = 1;
    imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (!vkOk(vkCreateImage(dev, &imgci, nullptr, &img), "vkCreateImage"))
        return false;
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo imai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex = findMemoryType(pd, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem = VK_NULL_HANDLE;
    if (!vkOk(vkAllocateMemory(dev, &imai, nullptr, &imem), "alloc image") || !vkOk(vkBindImageMemory(dev, img, imem, 0), "bind image"))
        return false;

    // Host-visible readback buffer.
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(kW) * kH * 4u;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (!vkOk(vkCreateBuffer(dev, &bci, nullptr, &buf), "vkCreateBuffer"))
        return false;
    VkMemoryRequirements bmr{};
    vkGetBufferMemoryRequirements(dev, buf, &bmr);
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bmai.allocationSize = bmr.size;
    bmai.memoryTypeIndex = findMemoryType(pd, bmr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    if (!vkOk(vkAllocateMemory(dev, &bmai, nullptr, &bmem), "alloc buffer") || !vkOk(vkBindBufferMemory(dev, buf, bmem, 0), "bind buffer"))
        return false;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = qf;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!vkOk(vkCreateCommandPool(dev, &pci, nullptr, &pool), "vkCreateCommandPool"))
        return false;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!vkOk(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers"))
        return false;

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbbi);

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkClearColorValue clear{};
    clear.float32[0] = 1.0f; // R=255 G=128 B=0 A=255
    clear.float32[1] = 128.0f / 255.0f;
    clear.float32[2] = 0.0f;
    clear.float32[3] = 1.0f;
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &b.subresourceRange);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (!vkOk(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit") || !vkOk(vkQueueWaitIdle(queue), "vkQueueWaitIdle"))
        return false;

    void *mapped = nullptr;
    if (!vkOk(vkMapMemory(dev, bmem, 0, bufSize, 0, &mapped), "vkMapMemory"))
        return false;
    const uint8_t *px = static_cast<const uint8_t *>(mapped);
    const size_t last = (static_cast<size_t>(kW) * kH - 1u) * 4u;
    const bool ok = px[0] == 255 && px[1] == 128 && px[2] == 0 && px[3] == 255 &&
                    px[last] == 255 && px[last + 1] == 128 && px[last + 2] == 0 && px[last + 3] == 255;
    std::printf("[vk] readback px0=%u,%u,%u,%u last=%u,%u,%u,%u -> %s\n", px[0], px[1], px[2], px[3],
                px[last], px[last + 1], px[last + 2], px[last + 3], ok ? "OK" : "MISMATCH");
    vkUnmapMemory(dev, bmem);

    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, bmem, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, imem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    std::printf("[vk] V0 selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
