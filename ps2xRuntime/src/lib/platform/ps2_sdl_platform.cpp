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
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic> // FIX-RDG4Q
#include <chrono> // FIX-RDG4Q
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

bool ps2xSdlWanted();
bool ps2xSdlInit(const char *title, int w, int h);
bool ps2xSdlActive();
bool ps2xSdlShouldClose();
void ps2xSdlForcePoll(); // FIX-RDG4Q
void ps2xSdlUploadFrame(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t strideBytes);
void ps2xSdlPresent();
const uint8_t *ps2xSdlLastFrameRgba(uint32_t &w, uint32_t &h, uint32_t &strideBytes); // FXC1
void ps2xSdlSetTitle(const char *t);
void ps2xSdlShutdown();
bool ps2xSdlReadPad(uint16_t *btnsActiveLow, uint8_t *sticks);
bool ps2xSdlAudioStart(void (*fill)(void *buffer, unsigned frames));
void ps2xSdlAudioStop();

namespace
{
    constexpr uint32_t kMaxFramesInFlight = 2;

    using AudioFillFn = void (*)(void *buffer, unsigned frames);

    struct SdlPlatformState
    {
        bool active = false;
        bool shouldClose = false;
        bool framebufferResized = false;

        SDL_Window *window = nullptr;
        SDL_Gamepad *gamepad = nullptr;
        SDL_JoystickID gamepadId = 0;
        std::atomic<bool> keys[SDL_SCANCODE_COUNT]{}; // FIX-RDG4Q: read from game thread without tearing

        SDL_AudioStream *audioStream = nullptr;
        AudioFillFn audioFill = nullptr;

        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE; // PROBE-RDG3B
        uint32_t vkValMsgCount = 0; // PROBE-RDG3B
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t queueFamily = UINT32_MAX;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D swapchainExtent{0, 0};
        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainImageViews;
        std::vector<VkSemaphore> renderFinishedSemaphores;

        VkCommandPool commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkFence> inFlightFences;
        uint32_t frameInFlight = 0;

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkDeviceSize stagingSize = 0;
        void *stagingMapped = nullptr;

        VkImage srcImage = VK_NULL_HANDLE;
        VkDeviceMemory srcImageMemory = VK_NULL_HANDLE;
        uint32_t srcW = 0;
        uint32_t srcH = 0;

        uint32_t frameW = 0;
        uint32_t frameH = 0;
        bool hasFrame = false;
        std::vector<uint8_t> cpuFrame;

        std::string shotDir;
        uint32_t shotEvery = 60;
        uint32_t presentCount = 0;
        uint32_t shotIndex = 0;

        uint32_t rdImageIndex = 0; // RDG2
        bool rdSuboptimal = false; // RDG2
        bool rdFrameOpen = false; // RDG2
        double rdLastBeginWaitMs = 0.0; // PROBE-RDG4P

        void reset()
        {
            active = false;
            shouldClose = false;
            framebufferResized = false;

            window = nullptr;
            gamepad = nullptr;
            gamepadId = 0;
            for (auto &k : keys)
            {
                k.store(false, std::memory_order_relaxed);
            }

            audioStream = nullptr;
            audioFill = nullptr;

            instance = VK_NULL_HANDLE;
            debugMessenger = VK_NULL_HANDLE;
            vkValMsgCount = 0;
            surface = VK_NULL_HANDLE;
            physicalDevice = VK_NULL_HANDLE;
            queueFamily = UINT32_MAX;
            device = VK_NULL_HANDLE;
            queue = VK_NULL_HANDLE;

            swapchain = VK_NULL_HANDLE;
            swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
            swapchainExtent = {0, 0};
            swapchainImages.clear();
            swapchainImageViews.clear();
            renderFinishedSemaphores.clear();

            commandPool = VK_NULL_HANDLE;
            commandBuffers.clear();
            imageAvailableSemaphores.clear();
            inFlightFences.clear();
            frameInFlight = 0;

            stagingBuffer = VK_NULL_HANDLE;
            stagingMemory = VK_NULL_HANDLE;
            stagingSize = 0;
            stagingMapped = nullptr;

            srcImage = VK_NULL_HANDLE;
            srcImageMemory = VK_NULL_HANDLE;
            srcW = 0;
            srcH = 0;

            frameW = 0;
            frameH = 0;
            hasFrame = false;
            cpuFrame.clear();

            shotDir.clear();
            shotEvery = 60;
            presentCount = 0;
            shotIndex = 0;

            rdImageIndex = 0;
            rdSuboptimal = false;
            rdFrameOpen = false;
            rdLastBeginWaitMs = 0.0;
        }
    };

    SdlPlatformState g_sdl;

    // PROBE-RDG4P
    bool rdvkLogEnabled()
    {
        static const bool enabled = []()
        {
            const char *env = std::getenv("PS2X_RDVK_LOG");
            return env && env[0] == '1';
        }();
        return enabled;
    }

    struct ScopedBeginWaitQpc
    {
        bool active = false;
#ifdef _WIN32
        LARGE_INTEGER start{};
#endif
        ScopedBeginWaitQpc() : active(rdvkLogEnabled())
        {
#ifdef _WIN32
            if (active)
                QueryPerformanceCounter(&start);
#endif
        }
        ~ScopedBeginWaitQpc()
        {
#ifdef _WIN32
            if (!active)
                return;
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            static const double invFreqMs = []()
            {
                LARGE_INTEGER freq{};
                QueryPerformanceFrequency(&freq);
                return freq.QuadPart > 0 ? (1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
            }();
            g_sdl.rdLastBeginWaitMs = static_cast<double>(end.QuadPart - start.QuadPart) * invFreqMs;
#endif
        }
    };

    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(g_sdl.physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    void DestroyStagingBuffer()
    {
        if (g_sdl.stagingMapped && g_sdl.stagingMemory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(g_sdl.device, g_sdl.stagingMemory);
            g_sdl.stagingMapped = nullptr;
        }
        if (g_sdl.stagingBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_sdl.device, g_sdl.stagingBuffer, nullptr);
            g_sdl.stagingBuffer = VK_NULL_HANDLE;
        }
        if (g_sdl.stagingMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_sdl.device, g_sdl.stagingMemory, nullptr);
            g_sdl.stagingMemory = VK_NULL_HANDLE;
        }
        g_sdl.stagingSize = 0;
    }

    bool EnsureStagingBuffer(VkDeviceSize needed)
    {
        if (needed <= g_sdl.stagingSize && g_sdl.stagingBuffer != VK_NULL_HANDLE)
        {
            return true;
        }
        vkDeviceWaitIdle(g_sdl.device);
        DestroyStagingBuffer();

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = needed;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g_sdl.device, &bci, nullptr, &g_sdl.stagingBuffer) != VK_SUCCESS)
        {
            return false;
        }

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(g_sdl.device, g_sdl.stagingBuffer, &req);
        uint32_t memType = FindMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memType == UINT32_MAX)
        {
            DestroyStagingBuffer();
            return false;
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType;
        if (vkAllocateMemory(g_sdl.device, &mai, nullptr, &g_sdl.stagingMemory) != VK_SUCCESS)
        {
            DestroyStagingBuffer();
            return false;
        }
        if (vkBindBufferMemory(g_sdl.device, g_sdl.stagingBuffer, g_sdl.stagingMemory, 0) != VK_SUCCESS)
        {
            DestroyStagingBuffer();
            return false;
        }
        if (vkMapMemory(g_sdl.device, g_sdl.stagingMemory, 0, needed, 0, &g_sdl.stagingMapped) != VK_SUCCESS)
        {
            DestroyStagingBuffer();
            return false;
        }
        g_sdl.stagingSize = needed;
        return true;
    }

    void DestroySrcImage()
    {
        if (g_sdl.srcImage != VK_NULL_HANDLE)
        {
            vkDestroyImage(g_sdl.device, g_sdl.srcImage, nullptr);
            g_sdl.srcImage = VK_NULL_HANDLE;
        }
        if (g_sdl.srcImageMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_sdl.device, g_sdl.srcImageMemory, nullptr);
            g_sdl.srcImageMemory = VK_NULL_HANDLE;
        }
        g_sdl.srcW = 0;
        g_sdl.srcH = 0;
    }

    bool EnsureSrcImage(uint32_t w, uint32_t h)
    {
        if (g_sdl.srcImage != VK_NULL_HANDLE && g_sdl.srcW == w && g_sdl.srcH == h)
        {
            return true;
        }
        vkDeviceWaitIdle(g_sdl.device);
        DestroySrcImage();

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g_sdl.device, &ici, nullptr, &g_sdl.srcImage) != VK_SUCCESS)
        {
            return false;
        }

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(g_sdl.device, g_sdl.srcImage, &req);
        uint32_t memType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX)
        {
            DestroySrcImage();
            return false;
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType;
        if (vkAllocateMemory(g_sdl.device, &mai, nullptr, &g_sdl.srcImageMemory) != VK_SUCCESS)
        {
            DestroySrcImage();
            return false;
        }
        if (vkBindImageMemory(g_sdl.device, g_sdl.srcImage, g_sdl.srcImageMemory, 0) != VK_SUCCESS)
        {
            DestroySrcImage();
            return false;
        }
        g_sdl.srcW = w;
        g_sdl.srcH = h;
        return true;
    }

    void DestroySwapchain()
    {
        for (VkSemaphore sem : g_sdl.renderFinishedSemaphores)
        {
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(g_sdl.device, sem, nullptr);
        }
        g_sdl.renderFinishedSemaphores.clear();

        for (VkImageView view : g_sdl.swapchainImageViews)
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(g_sdl.device, view, nullptr);
        }
        g_sdl.swapchainImageViews.clear();
        g_sdl.swapchainImages.clear();

        if (g_sdl.swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(g_sdl.device, g_sdl.swapchain, nullptr);
            g_sdl.swapchain = VK_NULL_HANDLE;
        }
    }

    bool CreateSwapchain(std::string &err)
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_sdl.window, &w, &h);
        if (w <= 0 || h <= 0)
        {
            g_sdl.swapchainExtent = {0, 0};
            return true;
        }

        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_sdl.physicalDevice, g_sdl.surface, &caps) != VK_SUCCESS)
        {
            err = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
            return false;
        }

        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_sdl.physicalDevice, g_sdl.surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fmtCount);
        if (fmtCount > 0)
        {
            vkGetPhysicalDeviceSurfaceFormatsKHR(g_sdl.physicalDevice, g_sdl.surface, &fmtCount, formats.data());
        }
        if (formats.empty())
        {
            err = "no surface formats available";
            return false;
        }

        VkSurfaceFormatKHR chosenFormat = formats[0];
        for (const auto &f : formats)
        {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosenFormat = f;
                break;
            }
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosenFormat = f;
            }
        }

        VkExtent2D extent{};
        if (caps.currentExtent.width != UINT32_MAX)
        {
            extent = caps.currentExtent;
        }
        else
        {
            extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        uint32_t imageCount = std::max(3u, caps.minImageCount);
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        {
            imageCount = caps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = g_sdl.surface;
        sci.minImageCount = imageCount;
        sci.imageFormat = chosenFormat.format;
        sci.imageColorSpace = chosenFormat.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(g_sdl.device, &sci, nullptr, &g_sdl.swapchain) != VK_SUCCESS)
        {
            err = "vkCreateSwapchainKHR failed";
            return false;
        }

        g_sdl.swapchainFormat = chosenFormat.format;
        g_sdl.swapchainExtent = extent;

        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(g_sdl.device, g_sdl.swapchain, &actualCount, nullptr);
        g_sdl.swapchainImages.resize(actualCount);
        vkGetSwapchainImagesKHR(g_sdl.device, g_sdl.swapchain, &actualCount, g_sdl.swapchainImages.data());

        g_sdl.swapchainImageViews.resize(actualCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < actualCount; ++i)
        {
            VkImageViewCreateInfo ivci{};
            ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image = g_sdl.swapchainImages[i];
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = g_sdl.swapchainFormat;
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ivci.subresourceRange.levelCount = 1;
            ivci.subresourceRange.layerCount = 1;
            if (vkCreateImageView(g_sdl.device, &ivci, nullptr, &g_sdl.swapchainImageViews[i]) != VK_SUCCESS)
            {
                err = "vkCreateImageView failed";
                return false;
            }
        }

        g_sdl.renderFinishedSemaphores.resize(actualCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < actualCount; ++i)
        {
            if (vkCreateSemaphore(g_sdl.device, &semInfo, nullptr, &g_sdl.renderFinishedSemaphores[i]) != VK_SUCCESS)
            {
                err = "vkCreateSemaphore (renderFinished) failed";
                return false;
            }
        }

        std::printf("[sdl] swapchain %ux%u\n", g_sdl.swapchainExtent.width, g_sdl.swapchainExtent.height);
        return true;
    }

    bool RecreateSwapchain(std::string &err)
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_sdl.window, &w, &h);
        if (w <= 0 || h <= 0)
        {
            g_sdl.swapchainExtent = {0, 0};
            return true;
        }
        vkDeviceWaitIdle(g_sdl.device);
        DestroySwapchain();
        return CreateSwapchain(err);
    }

    void OpenFirstGamepad()
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (!ids)
        {
            return;
        }
        if (count > 0 && !g_sdl.gamepad)
        {
            g_sdl.gamepad = SDL_OpenGamepad(ids[0]);
            if (g_sdl.gamepad)
            {
                g_sdl.gamepadId = ids[0];
                const char *name = SDL_GetGamepadName(g_sdl.gamepad);
                std::printf("[sdl] pad %s\n", name ? name : "unknown");
            }
        }
        SDL_free(ids);
    }

    // PROBE-RDG3B
    VKAPI_ATTR VkBool32 VKAPI_CALL VkValidationCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
        void *)
    {
        if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0)
        {
            return VK_FALSE;
        }
        const char *sevStr = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARNING";
        ++g_sdl.vkValMsgCount;
        if (g_sdl.vkValMsgCount <= 200)
        {
            std::printf("[vkval] %s %s\n", sevStr, (callbackData && callbackData->pMessage) ? callbackData->pMessage : "");
        }
        else if (g_sdl.vkValMsgCount == 201)
        {
            std::printf("[vkval] ... message limit (200) reached, counting remaining\n");
        }
        return VK_FALSE;
    }

    bool InitVulkan(std::string &err)
    {
        Uint32 extCount = 0;
        char const *const *sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
        if (!sdlExts || extCount == 0)
        {
            err = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
            return false;
        }

        // PROBE-RDG3B
        const char *valEnv = std::getenv("PS2X_VK_VALIDATE");
        const bool wantValidation = (valEnv && valEnv[0] == '1');
        std::vector<const char *> exts(sdlExts, sdlExts + extCount);
        std::vector<const char *> layers;
        bool hasDebugUtils = false;
        if (wantValidation)
        {
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> availLayers(layerCount);
            vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());
            for (const auto &lp : availLayers)
            {
                if (std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    break;
                }
            }

            uint32_t instExtCount = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
            std::vector<VkExtensionProperties> availExts(instExtCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, availExts.data());
            for (const auto &ep : availExts)
            {
                if (std::strcmp(ep.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                {
                    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    hasDebugUtils = true;
                    break;
                }
            }
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "PS2Recomp";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "PS2Recomp";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        VkDebugUtilsMessengerCreateInfoEXT dbgCi{};
        dbgCi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbgCi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbgCi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbgCi.pfnUserCallback = VkValidationCallback;

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &appInfo;
        ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ici.ppEnabledExtensionNames = exts.data();
        if (!layers.empty())
        {
            ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
            ici.ppEnabledLayerNames = layers.data();
        }
        if (wantValidation && hasDebugUtils)
        {
            ici.pNext = &dbgCi;
        }

        if (vkCreateInstance(&ici, nullptr, &g_sdl.instance) != VK_SUCCESS)
        {
            err = "vkCreateInstance failed";
            return false;
        }

        if (wantValidation && hasDebugUtils)
        {
            auto createDbg = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(g_sdl.instance, "vkCreateDebugUtilsMessengerEXT"));
            if (createDbg)
            {
                createDbg(g_sdl.instance, &dbgCi, nullptr, &g_sdl.debugMessenger);
            }
        }

        if (!SDL_Vulkan_CreateSurface(g_sdl.window, g_sdl.instance, nullptr, &g_sdl.surface))
        {
            err = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
            return false;
        }

        uint32_t pdCount = 0;
        vkEnumeratePhysicalDevices(g_sdl.instance, &pdCount, nullptr);
        if (pdCount == 0)
        {
            err = "no Vulkan physical devices found";
            return false;
        }
        std::vector<VkPhysicalDevice> pds(pdCount);
        vkEnumeratePhysicalDevices(g_sdl.instance, &pdCount, pds.data());

        int bestScore = -1;
        VkPhysicalDeviceProperties bestProps{};
        for (VkPhysicalDevice pd : pds)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);
            if (props.apiVersion < VK_API_VERSION_1_3)
            {
                continue;
            }

            uint32_t qfCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
            std::vector<VkQueueFamilyProperties> qfs(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());

            uint32_t chosenQf = UINT32_MAX;
            for (uint32_t i = 0; i < qfCount; ++i)
            {
                if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                    continue;
                VkBool32 presentSupport = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g_sdl.surface, &presentSupport);
                if (presentSupport == VK_TRUE)
                {
                    chosenQf = i;
                    break;
                }
            }
            if (chosenQf == UINT32_MAX)
                continue;

            uint32_t devExtCount = 0;
            vkEnumerateDeviceExtensionProperties(pd, nullptr, &devExtCount, nullptr);
            std::vector<VkExtensionProperties> devExts(devExtCount);
            vkEnumerateDeviceExtensionProperties(pd, nullptr, &devExtCount, devExts.data());
            bool hasSwapchain = false;
            for (const auto &e : devExts)
            {
                if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                {
                    hasSwapchain = true;
                    break;
                }
            }
            if (!hasSwapchain)
                continue;

            int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 100 : 10;
            if (score > bestScore)
            {
                bestScore = score;
                g_sdl.physicalDevice = pd;
                g_sdl.queueFamily = chosenQf;
                bestProps = props;
            }
        }

        if (g_sdl.physicalDevice == VK_NULL_HANDLE)
        {
            err = "no suitable Vulkan 1.3 physical device with graphics+present+swapchain";
            return false;
        }

        std::printf("[sdl] gpu=%s\n", bestProps.deviceName);

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = g_sdl.queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceVulkan13Features v13{};
        v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        v13.dynamicRendering = VK_TRUE;
        v13.synchronization2 = VK_TRUE;

        const char *deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &v13;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = deviceExts;

        if (vkCreateDevice(g_sdl.physicalDevice, &dci, nullptr, &g_sdl.device) != VK_SUCCESS)
        {
            err = "vkCreateDevice failed";
            return false;
        }
        vkGetDeviceQueue(g_sdl.device, g_sdl.queueFamily, 0, &g_sdl.queue);

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = g_sdl.queueFamily;
        if (vkCreateCommandPool(g_sdl.device, &cpci, nullptr, &g_sdl.commandPool) != VK_SUCCESS)
        {
            err = "vkCreateCommandPool failed";
            return false;
        }

        g_sdl.commandBuffers.resize(kMaxFramesInFlight);
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_sdl.commandPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = kMaxFramesInFlight;
        if (vkAllocateCommandBuffers(g_sdl.device, &cbai, g_sdl.commandBuffers.data()) != VK_SUCCESS)
        {
            err = "vkAllocateCommandBuffers failed";
            return false;
        }

        g_sdl.imageAvailableSemaphores.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
        g_sdl.inFlightFences.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (vkCreateSemaphore(g_sdl.device, &sci, nullptr, &g_sdl.imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(g_sdl.device, &fci, nullptr, &g_sdl.inFlightFences[i]) != VK_SUCCESS)
            {
                err = "failed to create per-frame sync objects";
                return false;
            }
        }

        return CreateSwapchain(err);
    }

    void ImageBarrier(VkCommandBuffer cmd,
                      VkImage image,
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
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    void WriteBmp24(const std::string &path, const uint8_t *rgba, uint32_t w, uint32_t h)
    {
        if (!rgba || w == 0 || h == 0)
        {
            return;
        }
        const uint32_t rowBytes = w * 3;
        const uint32_t padBytes = (4 - (rowBytes % 4)) % 4;
        const uint32_t stride = rowBytes + padBytes;
        const uint32_t imgSize = stride * h;
        const uint32_t fileSize = 54 + imgSize;

        std::vector<uint8_t> bmp(fileSize, 0);
        bmp[0] = 'B';
        bmp[1] = 'M';
        std::memcpy(&bmp[2], &fileSize, 4);
        const uint32_t offset = 54;
        std::memcpy(&bmp[10], &offset, 4);
        const uint32_t dibSize = 40;
        std::memcpy(&bmp[14], &dibSize, 4);
        const int32_t bmpW = static_cast<int32_t>(w);
        const int32_t bmpH = static_cast<int32_t>(h);
        std::memcpy(&bmp[18], &bmpW, 4);
        std::memcpy(&bmp[22], &bmpH, 4);
        const uint16_t planes = 1;
        const uint16_t bpp = 24;
        std::memcpy(&bmp[26], &planes, 2);
        std::memcpy(&bmp[28], &bpp, 2);
        std::memcpy(&bmp[34], &imgSize, 4);

        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *srcRow = rgba + static_cast<size_t>(h - 1 - y) * w * 4;
            uint8_t *dstRow = bmp.data() + 54 + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < w; ++x)
            {
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0];
            }
        }

        if (FILE *fp = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(bmp.data(), 1, bmp.size(), fp);
            std::fclose(fp);
        }
    }

    void SDLCALL SdlAudioCallback(void * /*userdata*/, SDL_AudioStream *stream, int additionalAmount, int /*totalAmount*/)
    {
        if (!stream || additionalAmount <= 0 || !g_sdl.audioFill)
        {
            return;
        }
        int totalFrames = additionalAmount / 4;
        int16_t temp[1024 * 2];
        while (totalFrames > 0)
        {
            int chunk = std::min(totalFrames, 1024);
            g_sdl.audioFill(temp, static_cast<unsigned>(chunk));
            SDL_PutAudioStreamData(stream, temp, chunk * 4);
            totalFrames -= chunk;
        }
    }
} // namespace

bool ps2xSdlWanted()
{
    const char *v = std::getenv("PS2X_FRONTEND");
    return v && std::strcmp(v, "sdl") == 0;
}

bool ps2xSdlInit(const char *title, int w, int h)
{
    if (g_sdl.active)
    {
        return true;
    }

    g_sdl.reset();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        std::printf("[sdl] init failed: %s\n", SDL_GetError());
        return false;
    }

    g_sdl.window = SDL_CreateWindow(title ? title : "PS2Recomp", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_sdl.window)
    {
        std::printf("[sdl] init failed: %s\n", SDL_GetError());
        ps2xSdlShutdown();
        return false;
    }

    std::string vkErr;
    if (!InitVulkan(vkErr))
    {
        std::printf("[sdl] init failed: %s\n", vkErr.c_str());
        ps2xSdlShutdown();
        return false;
    }

    OpenFirstGamepad();

    if (const char *shotEnv = std::getenv("PS2X_SDL_SHOT"))
    {
        if (shotEnv[0] != '\0')
        {
            g_sdl.shotDir = shotEnv;
        }
    }
    if (const char *everyEnv = std::getenv("PS2X_SDL_SHOT_EVERY"))
    {
        int parsed = std::atoi(everyEnv);
        if (parsed > 0)
        {
            g_sdl.shotEvery = static_cast<uint32_t>(parsed);
        }
    }

    g_sdl.active = true;
    return true;
}

bool ps2xSdlActive()
{
    return g_sdl.active && g_sdl.window != nullptr && g_sdl.device != VK_NULL_HANDLE;
}

namespace // FIX-RDG4Q
{
    std::chrono::steady_clock::time_point g_sdlLastPoll{};
    bool g_sdlHasPolled = false;

    void pollSdlEventsNow()
    {
        SDL_Event ev{};
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_sdl.shouldClose = true;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_sdl.framebufferResized = true;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (ev.key.scancode >= 0 && ev.key.scancode < SDL_SCANCODE_COUNT)
                {
                    g_sdl.keys[ev.key.scancode].store(ev.type == SDL_EVENT_KEY_DOWN, std::memory_order_relaxed);
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!g_sdl.gamepad)
                {
                    g_sdl.gamepad = SDL_OpenGamepad(ev.gdevice.which);
                    if (g_sdl.gamepad)
                    {
                        g_sdl.gamepadId = ev.gdevice.which;
                        const char *name = SDL_GetGamepadName(g_sdl.gamepad);
                        std::printf("[sdl] pad %s\n", name ? name : "unknown");
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_sdl.gamepad && ev.gdevice.which == g_sdl.gamepadId)
                {
                    SDL_CloseGamepad(g_sdl.gamepad);
                    g_sdl.gamepad = nullptr;
                    g_sdl.gamepadId = 0;
                    OpenFirstGamepad();
                }
                break;
            default:
                break;
            }
        }
        g_sdlLastPoll = std::chrono::steady_clock::now();
        g_sdlHasPolled = true;
    }
} // namespace

bool ps2xSdlShouldClose()
{
    // FIX-RDG4Q: throttle SDL_PollEvent on the ~1 kHz main loop (matching PS2X_IDLE_POLL_MS).
    if (g_sdl.shouldClose)
        return true;

    static const int64_t s_pollUs = []() {
        const char *e = std::getenv("PS2X_SDL_POLL_MS");
        const long v = e ? std::atol(e) : 4;
        return static_cast<int64_t>(v < 0 ? 0 : v) * 1000;
    }();
    const auto now = std::chrono::steady_clock::now();
    if (g_sdlHasPolled &&
        std::chrono::duration_cast<std::chrono::microseconds>(now - g_sdlLastPoll).count() < s_pollUs)
    {
        return g_sdl.shouldClose;
    }
    pollSdlEventsNow();
    return g_sdl.shouldClose;
}

void ps2xSdlForcePoll() // FIX-RDG4Q
{
    if (!ps2xSdlActive())
        return;
    pollSdlEventsNow();
}

void ps2xSdlUploadFrame(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t strideBytes)
{
    if (!ps2xSdlActive() || !rgba || w == 0 || h == 0)
    {
        return;
    }

    const uint32_t rowBytes = w * 4;
    const uint32_t srcStride = (strideBytes == 0) ? rowBytes : strideBytes;
    const VkDeviceSize totalBytes = static_cast<VkDeviceSize>(rowBytes) * h;

    if (!EnsureStagingBuffer(totalBytes))
    {
        return;
    }

    g_sdl.cpuFrame.resize(static_cast<size_t>(totalBytes));
    auto *dstStaging = static_cast<uint8_t *>(g_sdl.stagingMapped);
    if (srcStride == rowBytes)
    {
        std::memcpy(dstStaging, rgba, static_cast<size_t>(totalBytes));
        std::memcpy(g_sdl.cpuFrame.data(), rgba, static_cast<size_t>(totalBytes));
    }
    else
    {
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *srcRow = rgba + static_cast<size_t>(y) * srcStride;
            std::memcpy(dstStaging + static_cast<size_t>(y) * rowBytes, srcRow, rowBytes);
            std::memcpy(g_sdl.cpuFrame.data() + static_cast<size_t>(y) * rowBytes, srcRow, rowBytes);
        }
    }

    g_sdl.frameW = w;
    g_sdl.frameH = h;
    g_sdl.hasFrame = true;
}

const uint8_t *ps2xSdlLastFrameRgba(uint32_t &w, uint32_t &h, uint32_t &strideBytes) // FXC1
{
    if (!ps2xSdlActive() || !g_sdl.hasFrame || g_sdl.cpuFrame.empty() || g_sdl.frameW == 0 || g_sdl.frameH == 0)
    {
        w = 0;
        h = 0;
        strideBytes = 0;
        return nullptr;
    }
    w = g_sdl.frameW;
    h = g_sdl.frameH;
    strideBytes = g_sdl.frameW * 4u;
    return g_sdl.cpuFrame.data();
}

void ps2xSdlPresent()
{
    if (!ps2xSdlActive())
    {
        return;
    }

    if (g_sdl.framebufferResized || g_sdl.swapchainExtent.width == 0 || g_sdl.swapchainExtent.height == 0)
    {
        g_sdl.framebufferResized = false;
        std::string err;
        if (!RecreateSwapchain(err))
        {
            return;
        }
        if (g_sdl.swapchainExtent.width == 0 || g_sdl.swapchainExtent.height == 0)
        {
            return;
        }
    }

    const uint32_t frameIdx = g_sdl.frameInFlight;
    vkWaitForFences(g_sdl.device, 1, &g_sdl.inFlightFences[frameIdx], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acqRes = vkAcquireNextImageKHR(
        g_sdl.device,
        g_sdl.swapchain,
        UINT64_MAX,
        g_sdl.imageAvailableSemaphores[frameIdx],
        VK_NULL_HANDLE,
        &imageIndex);
    if (acqRes == VK_ERROR_OUT_OF_DATE_KHR)
    {
        g_sdl.framebufferResized = true;
        return;
    }
    if (acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR)
    {
        return;
    }

    const bool drawFrame = g_sdl.hasFrame && g_sdl.frameW > 0 && g_sdl.frameH > 0 &&
                           g_sdl.stagingBuffer != VK_NULL_HANDLE &&
                           EnsureSrcImage(g_sdl.frameW, g_sdl.frameH);

    vkResetFences(g_sdl.device, 1, &g_sdl.inFlightFences[frameIdx]);
    VkCommandBuffer cmd = g_sdl.commandBuffers[frameIdx];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);

    VkImage swapImg = g_sdl.swapchainImages[imageIndex];

    if (!drawFrame)
    {
        ImageBarrier(cmd, swapImg,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = g_sdl.swapchainImageViews[imageIndex];
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color.float32[0] = 0.0f;
        colorAtt.clearValue.color.float32[1] = 0.0f;
        colorAtt.clearValue.color.float32[2] = 0.0f;
        colorAtt.clearValue.color.float32[3] = 1.0f;

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, g_sdl.swapchainExtent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &colorAtt;

        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);

        ImageBarrier(cmd, swapImg,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    else
    {
        ImageBarrier(cmd, g_sdl.srcImage,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {g_sdl.frameW, g_sdl.frameH, 1};
        vkCmdCopyBufferToImage(cmd, g_sdl.stagingBuffer, g_sdl.srcImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        ImageBarrier(cmd, g_sdl.srcImage,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        ImageBarrier(cmd, swapImg,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue black{};
        black.float32[3] = 1.0f;
        VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &clearRange);

        const float srcAspect = static_cast<float>(g_sdl.frameW) / static_cast<float>(g_sdl.frameH);
        const float dstAspect = static_cast<float>(g_sdl.swapchainExtent.width) / static_cast<float>(g_sdl.swapchainExtent.height);
        int32_t dstX = 0;
        int32_t dstY = 0;
        int32_t dstW = static_cast<int32_t>(g_sdl.swapchainExtent.width);
        int32_t dstH = static_cast<int32_t>(g_sdl.swapchainExtent.height);
        if (dstAspect > srcAspect)
        {
            dstW = std::clamp(static_cast<int32_t>(static_cast<float>(g_sdl.swapchainExtent.height) * srcAspect + 0.5f),
                              1, static_cast<int32_t>(g_sdl.swapchainExtent.width));
            dstX = (static_cast<int32_t>(g_sdl.swapchainExtent.width) - dstW) / 2;
        }
        else
        {
            dstH = std::clamp(static_cast<int32_t>(static_cast<float>(g_sdl.swapchainExtent.width) / srcAspect + 0.5f),
                              1, static_cast<int32_t>(g_sdl.swapchainExtent.height));
            dstY = (static_cast<int32_t>(g_sdl.swapchainExtent.height) - dstH) / 2;
        }

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(g_sdl.frameW), static_cast<int32_t>(g_sdl.frameH), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {dstX, dstY, 0};
        blit.dstOffsets[1] = {dstX + dstW, dstY + dstH, 1};
        vkCmdBlitImage(cmd,
                       g_sdl.srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        ImageBarrier(cmd, swapImg,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo waitSem{};
    waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSem.semaphore = g_sdl.imageAvailableSemaphores[frameIdx];
    waitSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSemaphoreSubmitInfo sigSem{};
    sigSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sigSem.semaphore = g_sdl.renderFinishedSemaphores[imageIndex];
    sigSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSem;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &sigSem;

    if (vkQueueSubmit2(g_sdl.queue, 1, &submitInfo, g_sdl.inFlightFences[frameIdx]) != VK_SUCCESS)
    {
        return;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &g_sdl.renderFinishedSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &g_sdl.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presRes = vkQueuePresentKHR(g_sdl.queue, &presentInfo);
    if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR || acqRes == VK_SUBOPTIMAL_KHR)
    {
        g_sdl.framebufferResized = true;
    }

    g_sdl.frameInFlight = (g_sdl.frameInFlight + 1) % kMaxFramesInFlight;
    ++g_sdl.presentCount;

    if (!g_sdl.shotDir.empty() && g_sdl.hasFrame && !g_sdl.cpuFrame.empty() &&
        (g_sdl.presentCount % g_sdl.shotEvery) == 0)
    {
        std::error_code ec;
        std::filesystem::create_directories(g_sdl.shotDir, ec);
        char fname[64];
        std::snprintf(fname, sizeof(fname), "f%06u.bmp", g_sdl.shotIndex++);
        std::filesystem::path outPath = std::filesystem::path(g_sdl.shotDir) / fname;
        WriteBmp24(outPath.string(), g_sdl.cpuFrame.data(), g_sdl.frameW, g_sdl.frameH);
    }
}

void ps2xSdlSetTitle(const char *t)
{
    if (g_sdl.window && t)
    {
        SDL_SetWindowTitle(g_sdl.window, t);
    }
}

void ps2xSdlShutdown()
{
    if (g_sdl.audioStream)
    {
        SDL_DestroyAudioStream(g_sdl.audioStream);
        g_sdl.audioStream = nullptr;
    }
    g_sdl.audioFill = nullptr;

    if (g_sdl.device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(g_sdl.device);
        DestroySrcImage();
        DestroyStagingBuffer();
        DestroySwapchain();

        for (VkFence fence : g_sdl.inFlightFences)
        {
            if (fence != VK_NULL_HANDLE)
                vkDestroyFence(g_sdl.device, fence, nullptr);
        }
        g_sdl.inFlightFences.clear();

        for (VkSemaphore sem : g_sdl.imageAvailableSemaphores)
        {
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(g_sdl.device, sem, nullptr);
        }
        g_sdl.imageAvailableSemaphores.clear();

        if (g_sdl.commandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(g_sdl.device, g_sdl.commandPool, nullptr);
            g_sdl.commandPool = VK_NULL_HANDLE;
        }

        vkDestroyDevice(g_sdl.device, nullptr);
        g_sdl.device = VK_NULL_HANDLE;
    }

    if (g_sdl.surface != VK_NULL_HANDLE && g_sdl.instance != VK_NULL_HANDLE)
    {
        SDL_Vulkan_DestroySurface(g_sdl.instance, g_sdl.surface, nullptr);
        g_sdl.surface = VK_NULL_HANDLE;
    }

    if (g_sdl.instance != VK_NULL_HANDLE)
    {
        // PROBE-RDG3B
        if (g_sdl.vkValMsgCount > 200)
        {
            std::printf("[vkval] total=%u suppressed=%u\n", g_sdl.vkValMsgCount, g_sdl.vkValMsgCount - 200);
        }
        if (g_sdl.debugMessenger != VK_NULL_HANDLE)
        {
            auto destroyDbg = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(g_sdl.instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyDbg)
            {
                destroyDbg(g_sdl.instance, g_sdl.debugMessenger, nullptr);
            }
            g_sdl.debugMessenger = VK_NULL_HANDLE;
        }
        vkDestroyInstance(g_sdl.instance, nullptr);
        g_sdl.instance = VK_NULL_HANDLE;
    }

    if (g_sdl.gamepad)
    {
        SDL_CloseGamepad(g_sdl.gamepad);
        g_sdl.gamepad = nullptr;
        g_sdl.gamepadId = 0;
    }

    if (g_sdl.window)
    {
        SDL_DestroyWindow(g_sdl.window);
        g_sdl.window = nullptr;
    }

    SDL_Quit();
    g_sdl.active = false;
    g_sdl.shouldClose = false;
    g_sdlHasPolled = false; // FIX-RDG4Q
    g_sdl.hasFrame = false;
    g_sdl.cpuFrame.clear();
}

bool ps2xSdlReadPad(uint16_t *btnsActiveLow, uint8_t *sticks)
{
    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    uint16_t btns = 0xFFFFu;
    uint8_t localSticks[4] = {0x80, 0x80, 0x80, 0x80};
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (g_sdl.gamepad)
    {
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))
            clearBit(PAD_UP);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
            clearBit(PAD_DOWN);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
            clearBit(PAD_LEFT);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
            clearBit(PAD_RIGHT);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_SOUTH))
            clearBit(PAD_CROSS);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_EAST))
            clearBit(PAD_CIRCLE);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_WEST))
            clearBit(PAD_SQUARE);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_NORTH))
            clearBit(PAD_TRIANGLE);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
            clearBit(PAD_L1);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
            clearBit(PAD_R1);
        if (SDL_GetGamepadAxis(g_sdl.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16383)
            clearBit(PAD_L2);
        if (SDL_GetGamepadAxis(g_sdl.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16383)
            clearBit(PAD_R2);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_START))
            clearBit(PAD_START);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_BACK))
            clearBit(PAD_SELECT);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK))
            clearBit(PAD_L3);
        if (SDL_GetGamepadButton(g_sdl.gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK))
            clearBit(PAD_R3);

        auto normAxis = [](Sint16 raw) -> float
        {
            float f = static_cast<float>(raw) / 32767.0f;
            return std::clamp(f, -1.0f, 1.0f);
        };
        auto stick = [](float v) -> uint8_t
        {
            constexpr float kDead = 0.15f;
            if (v > -kDead && v < kDead)
                return 128;
            const float s = (v > 0.0f ? v - kDead : v + kDead) / (1.0f - kDead);
            const float o = 128.0f + s * 127.0f;
            return static_cast<uint8_t>(o < 0.0f ? 0.0f : (o > 255.0f ? 255.0f : o));
        };

        localSticks[0] = stick(normAxis(SDL_GetGamepadAxis(g_sdl.gamepad, SDL_GAMEPAD_AXIS_RIGHTX)));
        localSticks[1] = stick(normAxis(SDL_GetGamepadAxis(g_sdl.gamepad, SDL_GAMEPAD_AXIS_RIGHTY)));
        localSticks[2] = stick(normAxis(SDL_GetGamepadAxis(g_sdl.gamepad, SDL_GAMEPAD_AXIS_LEFTX)));
        localSticks[3] = stick(normAxis(SDL_GetGamepadAxis(g_sdl.gamepad, SDL_GAMEPAD_AXIS_LEFTY)));
    }

    auto keyAxis = [](uint8_t &out, SDL_Scancode neg, SDL_Scancode pos)
    {
        if (g_sdl.keys[neg])
            out = 0;
        else if (g_sdl.keys[pos])
            out = 255;
    };
    keyAxis(localSticks[2], SDL_SCANCODE_A, SDL_SCANCODE_D);
    keyAxis(localSticks[3], SDL_SCANCODE_W, SDL_SCANCODE_S);
    keyAxis(localSticks[0], SDL_SCANCODE_J, SDL_SCANCODE_L);
    keyAxis(localSticks[1], SDL_SCANCODE_I, SDL_SCANCODE_K);

    if (g_sdl.keys[SDL_SCANCODE_UP])
        clearBit(PAD_UP);
    if (g_sdl.keys[SDL_SCANCODE_DOWN])
        clearBit(PAD_DOWN);
    if (g_sdl.keys[SDL_SCANCODE_LEFT])
        clearBit(PAD_LEFT);
    if (g_sdl.keys[SDL_SCANCODE_RIGHT])
        clearBit(PAD_RIGHT);
    if (g_sdl.keys[SDL_SCANCODE_X] || g_sdl.keys[SDL_SCANCODE_SPACE])
        clearBit(PAD_CROSS);
    if (g_sdl.keys[SDL_SCANCODE_C] || g_sdl.keys[SDL_SCANCODE_ESCAPE])
        clearBit(PAD_CIRCLE);
    if (g_sdl.keys[SDL_SCANCODE_Z] || g_sdl.keys[SDL_SCANCODE_KP_0])
        clearBit(PAD_SQUARE);
    if (g_sdl.keys[SDL_SCANCODE_V] || g_sdl.keys[SDL_SCANCODE_KP_1])
        clearBit(PAD_TRIANGLE);
    if (g_sdl.keys[SDL_SCANCODE_Q])
        clearBit(PAD_L1);
    if (g_sdl.keys[SDL_SCANCODE_E])
        clearBit(PAD_R1);
    if (g_sdl.keys[SDL_SCANCODE_LSHIFT])
        clearBit(PAD_L2);
    if (g_sdl.keys[SDL_SCANCODE_RSHIFT])
        clearBit(PAD_R2);
    if (g_sdl.keys[SDL_SCANCODE_RETURN])
        clearBit(PAD_START);
    if (g_sdl.keys[SDL_SCANCODE_TAB])
        clearBit(PAD_SELECT);

    if (btnsActiveLow)
        *btnsActiveLow = btns;
    if (sticks)
        std::memcpy(sticks, localSticks, 4);
    return true;
}

bool ps2xSdlAudioStart(void (*fill)(void *buffer, unsigned frames))
{
    if (!ps2xSdlActive() || !fill)
    {
        return false;
    }
    ps2xSdlAudioStop();

    g_sdl.audioFill = fill;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 48000;

    g_sdl.audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, SdlAudioCallback, nullptr);
    if (!g_sdl.audioStream)
    {
        g_sdl.audioFill = nullptr;
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(g_sdl.audioStream))
    {
        SDL_DestroyAudioStream(g_sdl.audioStream);
        g_sdl.audioStream = nullptr;
        g_sdl.audioFill = nullptr;
        return false;
    }
    std::printf("[sdl] audio started\n");
    return true;
}

void ps2xSdlAudioStop()
{
    if (g_sdl.audioStream)
    {
        SDL_DestroyAudioStream(g_sdl.audioStream);
        g_sdl.audioStream = nullptr;
    }
    g_sdl.audioFill = nullptr;
}

// RDG2: Vulkan accessors and frame protocol for rd_gfx_vk.
VkInstance ps2xSdlVkInstance() { return g_sdl.instance; }
VkPhysicalDevice ps2xSdlVkPhysicalDevice() { return g_sdl.physicalDevice; }
VkDevice ps2xSdlVkDevice() { return g_sdl.device; }
VkQueue ps2xSdlVkQueue() { return g_sdl.queue; }
uint32_t ps2xSdlVkQueueFamily() { return g_sdl.queueFamily; }
VkFormat ps2xSdlVkSwapFormat() { return g_sdl.swapchainFormat; }
uint32_t ps2xSdlVkFramesInFlight() { return kMaxFramesInFlight; }
double ps2xSdlVkLastBeginWaitMs() { return g_sdl.rdLastBeginWaitMs; } // PROBE-RDG4P

bool ps2xSdlVkBeginRdFrame(VkCommandBuffer *cb, VkImage *swapImage, uint32_t *w, uint32_t *h, uint32_t *frameIdx)
{
    ScopedBeginWaitQpc waitProbe; // PROBE-RDG4P
    if (!ps2xSdlActive())
    {
        return false;
    }

    if (g_sdl.framebufferResized || g_sdl.swapchainExtent.width == 0 || g_sdl.swapchainExtent.height == 0)
    {
        g_sdl.framebufferResized = false;
        std::string err;
        if (!RecreateSwapchain(err))
        {
            return false;
        }
        if (g_sdl.swapchainExtent.width == 0 || g_sdl.swapchainExtent.height == 0)
        {
            return false;
        }
    }

    const uint32_t curFrame = g_sdl.frameInFlight;
    vkWaitForFences(g_sdl.device, 1, &g_sdl.inFlightFences[curFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acqRes = vkAcquireNextImageKHR(
        g_sdl.device,
        g_sdl.swapchain,
        UINT64_MAX,
        g_sdl.imageAvailableSemaphores[curFrame],
        VK_NULL_HANDLE,
        &imageIndex);
    if (acqRes == VK_ERROR_OUT_OF_DATE_KHR)
    {
        g_sdl.framebufferResized = true;
        return false;
    }
    if (acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR)
    {
        return false;
    }

    vkResetFences(g_sdl.device, 1, &g_sdl.inFlightFences[curFrame]);
    VkCommandBuffer cmd = g_sdl.commandBuffers[curFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    {
        return false;
    }

    g_sdl.rdImageIndex = imageIndex;
    g_sdl.rdSuboptimal = (acqRes == VK_SUBOPTIMAL_KHR);
    g_sdl.rdFrameOpen = true;

    if (cb)
        *cb = cmd;
    if (swapImage)
        *swapImage = g_sdl.swapchainImages[imageIndex];
    if (w)
        *w = g_sdl.swapchainExtent.width;
    if (h)
        *h = g_sdl.swapchainExtent.height;
    if (frameIdx)
        *frameIdx = curFrame;

    return true;
}

void ps2xSdlVkEndRdFrame()
{
    if (!ps2xSdlActive() || !g_sdl.rdFrameOpen)
    {
        return;
    }
    g_sdl.rdFrameOpen = false;

    const uint32_t curFrame = g_sdl.frameInFlight;
    const uint32_t imageIndex = g_sdl.rdImageIndex;
    VkCommandBuffer cmd = g_sdl.commandBuffers[curFrame];

    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo waitSem{};
    waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSem.semaphore = g_sdl.imageAvailableSemaphores[curFrame];
    waitSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSemaphoreSubmitInfo sigSem{};
    sigSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sigSem.semaphore = g_sdl.renderFinishedSemaphores[imageIndex];
    sigSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSem;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &sigSem;

    if (vkQueueSubmit2(g_sdl.queue, 1, &submitInfo, g_sdl.inFlightFences[curFrame]) != VK_SUCCESS)
    {
        return;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &g_sdl.renderFinishedSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &g_sdl.swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presRes = vkQueuePresentKHR(g_sdl.queue, &presentInfo);
    if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR || g_sdl.rdSuboptimal)
    {
        g_sdl.framebufferResized = true;
    }

    g_sdl.frameInFlight = (g_sdl.frameInFlight + 1) % kMaxFramesInFlight;
    ++g_sdl.presentCount;
}

#endif // PS2X_HAS_SDL3
