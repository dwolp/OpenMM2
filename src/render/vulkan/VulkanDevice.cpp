// Vulkan backend.
//
// Baseline: Vulkan 1.1 (negative viewport height for a y-up clip space) with
// classic render passes rather than dynamic rendering, so it runs on every
// Vulkan-capable GPU including older and mobile-class drivers.
//
// Per frame (2 in flight):
//   [upload cmd]  staging copies + layout transitions issued during the frame
//   [main cmd]    scene pass (offscreen, MSAA + resolve, render scale)
//                 output pass (composite scene onto the swapchain, overlays)
//                 optional capture copy
// Resources created outside a frame (level loading) are batched into one
// command buffer that is submitted when the next frame starts, or when the
// staging memory grows large.

#include "core/File.h"
#include "core/Log.h"
#include "platform/Window.h"
#include "render/Device.h"
#include "render/GpuConstants.h"
#include "render/HandleTable.h"
#include "render/ImageUtil.h"
#include "render/Projection.h"
#include "render/ShaderBlobs.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <unordered_map>

namespace mm2::render {
namespace {

constexpr std::uint32_t kFramesInFlight = 2;
constexpr VkDeviceSize kRingChunkSize = 4u << 20;
constexpr VkDeviceSize kStagingChunkSize = 16u << 20;
constexpr VkDeviceSize kLoadFlushThreshold = 128u << 20;
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

const char* vkResultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    default: return "VkResult";
    }
}

#define VK_CHECK_INIT(expr)                                                                                          \
    do {                                                                                                              \
        if (VkResult r_ = (expr); r_ != VK_SUCCESS) {                                                                 \
            *error = std::format("{} failed: {} ({})", #expr, vkResultName(r_), static_cast<int>(r_));               \
            return false;                                                                                             \
        }                                                                                                             \
    } while (0)

void logVk(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        log::error("vulkan: {} failed: {}", what, vkResultName(r));
}

VkCompareOp toVk(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS_OR_EQUAL;
}

VkSamplerAddressMode toVk(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

std::vector<std::uint32_t> toWords(std::span<const std::uint8_t> spv) {
    std::vector<std::uint32_t> words((spv.size() + 3) / 4);
    std::memcpy(words.data(), spv.data(), spv.size());
    return words;
}

struct VkTex {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0, mips = 1;
};

struct VkBuf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

// Host-visible, persistently mapped linear allocator reset every frame.
struct Ring {
    struct Chunk {
        std::uint32_t handle = 0; // id in m_buffers
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize offset = 0;
        VkDescriptorSet uboSet = VK_NULL_HANDLE; // uniform rings only
    };
    VkBufferUsageFlags usage = 0;
    std::vector<Chunk> chunks;
    std::size_t current = 0;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE;
    bool uploadRecording = false;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    Ring geometry; // vertex + index
    Ring uniforms;
    Ring staging;
    std::vector<std::function<void()>> deletions;
    bool captured = false;
};

struct SwapImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessenger(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++*static_cast<std::uint32_t*>(user);
        log::error("vulkan: {}", data->pMessage ? data->pMessage : "");
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log::warn("vulkan: {}", data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(platform::Window& window) : m_window(window) {}
    ~VulkanDevice() override;

    bool init(const DeviceCreateInfo& info, std::string* error);

    const DeviceInfo& info() const override { return m_info; }

    TextureHandle createTexture(const TextureDesc& desc, std::span<const TextureData> mips) override;
    void updateTexture(TextureHandle texture, std::uint32_t mip, const Rect& region, const void* data,
                       std::uint32_t rowPitch) override;
    void destroyTexture(TextureHandle texture) override;
    BufferHandle createBuffer(BufferKind kind, std::size_t size, const void* data) override;
    void updateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) override;
    void destroyBuffer(BufferHandle buffer) override;
    BufferSlice uploadTransient(BufferKind kind, std::span<const std::byte> data) override;

    void applySettings(const DisplaySettings& settings) override;
    void notifyResized() override { m_resizeHint = true; }

    bool beginFrame() override;
    Extent2D outputExtent() const override { return m_output; }
    Extent2D sceneExtent() const override { return m_sceneExtent; }
    void beginScene(const ClearValues& clear) override;
    void endScene() override;
    void beginOverlay(const Vec4& clearColor) override;
    void endOverlay() override;
    void endFrame() override;

    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect* rect) override;
    void clear(const ClearValues& values) override;
    void setFrameConstants(const FrameConstants& constants) override;
    void draw(const DrawCall& call) override;

    void requestCapture() override { m_captureRequested = true; }
    bool readCapture(Image& out) override;
    void waitIdle() override;
    const FrameStats& stats() const override { return m_stats; }

private:
    enum class Pass { None, Scene, Overlay };

    bool createInstance(bool validation, std::string* error);
    bool pickPhysicalDevice(int preferred, std::string* error);
    bool createLogicalDevice(std::string* error);
    bool createLayouts(std::string* error);
    void loadPipelineCache();
    void savePipelineCache();
    VkFormat pickDepthFormat() const;

    bool createSwapchain();
    void destroySwapchainResources();
    bool createSceneTargets();
    void destroySceneTargets();
    void createOutputRenderPass();
    void createSceneRenderPass();
    void recreateAll();

    VkPipeline pipeline(const PipelineState& state, Pass pass);
    VkPipeline compositePipeline();
    void destroyPipelines(bool sceneOnly);
    VkSampler sampler(const SamplerDesc& desc);
    VkDescriptorSet textureSet(std::uint32_t textureId, VkImageView view, const SamplerDesc& desc);
    VkDescriptorSet allocateSet(VkDescriptorSetLayout layout, VkDescriptorPool* poolOut);
    void forgetTextureSets(std::uint32_t textureId);

    // Uploads
    VkCommandBuffer uploadCommands();
    std::pair<VkBuffer, VkDeviceSize> stage(const void* data, VkDeviceSize size, VkDeviceSize align = 16);
    void flushLoadUploads();

    std::pair<Ring::Chunk*, VkDeviceSize> ringAlloc(Ring& ring, VkDeviceSize size, VkDeviceSize align);
    void resetRing(Ring& ring);
    void destroyRing(Ring& ring);
    void deferDelete(std::function<void()> fn);
    void setDebugName(VkObjectType type, std::uint64_t handle, const std::string& name);

    void bindDefaultViewport();
    void applyViewport();

    platform::Window& m_window;
    DisplaySettings m_settings;
    DeviceInfo m_info;
    FrameStats m_stats;
    std::uint32_t m_validationErrors = 0;
    std::filesystem::path m_pipelineCachePath;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    bool m_debugUtils = false;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_gpuProps{};
    VkDevice m_device = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0, m_presentFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE, m_presentQueue = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    bool m_anisotropySupported = false;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_texSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_pools;
    VkShaderModule m_meshVs = VK_NULL_HANDLE, m_meshFs = VK_NULL_HANDLE;
    VkShaderModule m_overlayVs = VK_NULL_HANDLE, m_overlayFs = VK_NULL_HANDLE;
    VkShaderModule m_compVs = VK_NULL_HANDLE, m_compFs = VK_NULL_HANDLE;

    // Swapchain / output
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
    bool m_swapCanCopy = false;
    std::vector<SwapImage> m_swapImages;
    VkRenderPass m_outputPass = VK_NULL_HANDLE;
    Extent2D m_output;
    bool m_swapchainDirty = true; // must recreate (out of date, suboptimal, settings)
    bool m_resizeHint = false;     // window reported a resize: recreate if the surface size changed

    // Scene targets
    VkRenderPass m_scenePass = VK_NULL_HANDLE;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    Extent2D m_sceneExtent;
    VkTex m_sceneColor, m_sceneMsaa, m_sceneDepth;
    VkFramebuffer m_sceneFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet m_sceneSet = VK_NULL_HANDLE;
    VkDescriptorPool m_sceneSetPool = VK_NULL_HANDLE;

    std::unordered_map<std::uint64_t, VkPipeline> m_pipelines;
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    std::unordered_map<std::uint32_t, VkSampler> m_samplers;
    struct CachedSet {
        VkDescriptorSet set;
        VkDescriptorPool pool;
    };
    std::unordered_map<std::uint64_t, CachedSet> m_texSets;

    detail::HandleTable<VkTex> m_textures;
    detail::HandleTable<VkBuf> m_buffers;
    TextureHandle m_white;

    std::array<Frame, kFramesInFlight> m_frames;
    std::uint32_t m_frameIndex = 0;
    std::uint32_t m_imageIndex = 0;
    bool m_inFrame = false;
    Pass m_pass = Pass::None;
    bool m_sceneRendered = false;

    // Uploads issued outside a frame.
    VkCommandPool m_loadPool = VK_NULL_HANDLE;
    VkCommandBuffer m_loadCmd = VK_NULL_HANDLE;
    VkFence m_loadFence = VK_NULL_HANDLE;
    bool m_loadRecording = false;
    std::vector<VkBuf> m_loadStaging;
    VkDeviceSize m_loadStagingBytes = 0;

    // Draw state
    Viewport m_viewport;
    Rect m_scissor;
    bool m_scissorSet = false;
    VkPipeline m_boundPipeline = VK_NULL_HANDLE;
    VkBuffer m_boundVb = VK_NULL_HANDLE;
    VkDeviceSize m_boundVbOffset = ~VkDeviceSize{0};
    VkBuffer m_boundIb = VK_NULL_HANDLE;
    VkDeviceSize m_boundIbOffset = ~VkDeviceSize{0};
    VkIndexType m_boundIbType = VK_INDEX_TYPE_MAX_ENUM;
    VkDescriptorSet m_boundTexSet[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool m_frameSetBound = false;

    // Capture
    bool m_captureRequested = false;
    bool m_captureReady = false;
    std::uint32_t m_captureFrame = 0;
    VkBuf m_captureBuffer;
    Extent2D m_captureExtent;
};

// ---------------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------------

bool VulkanDevice::createInstance(bool validation, std::string* error) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!gipa) {
        *error = std::format("no Vulkan loader: {}", SDL_GetError());
        return false;
    }
    volkInitializeCustom(gipa);
    const std::uint32_t loaderVersion = volkGetInstanceVersion();
    if (loaderVersion < VK_API_VERSION_1_1) {
        *error = "Vulkan 1.1 loader required";
        return false;
    }

    std::uint32_t sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) {
        *error = std::format("SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(sdlExts, sdlExts + sdlExtCount);

    std::uint32_t availCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> avail(availCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, avail.data());
    auto hasExt = [&](const char* name) {
        return std::ranges::any_of(avail, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    VkInstanceCreateFlags flags = 0;
    if (hasExt(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    std::vector<const char*> layers;
    if (validation) {
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> props(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, props.data());
        const bool haveLayer = std::ranges::any_of(props, [](const VkLayerProperties& p) {
            return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (haveLayer)
            layers.push_back("VK_LAYER_KHRONOS_validation");
        else
            log::warn("vulkan: validation requested but VK_LAYER_KHRONOS_validation is not installed");
    }
    if ((validation || hasExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) && hasExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        m_debugUtils = true;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "OpenMM2";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "OpenMM2";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VK_CHECK_INIT(vkCreateInstance(&ci, nullptr, &m_instance));
    volkLoadInstanceOnly(m_instance);

    if (m_debugUtils && !layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = debugMessenger;
        mi.pUserData = &m_validationErrors;
        vkCreateDebugUtilsMessengerEXT(m_instance, &mi, nullptr, &m_messenger);
        log::info("vulkan: validation layer enabled");
    }

    if (!SDL_Vulkan_CreateSurface(m_window.sdl(), m_instance, nullptr, &m_surface)) {
        *error = std::format("SDL_Vulkan_CreateSurface: {}", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanDevice::pickPhysicalDevice(int preferred, std::string* error) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(m_instance, &count, gpus.data());
    if (gpus.empty()) {
        *error = "no Vulkan devices";
        return false;
    }

    struct Candidate {
        VkPhysicalDevice gpu;
        std::uint32_t graphics, present;
        int score;
        int index;
    };
    std::vector<Candidate> usable;
    std::string rejected;
    for (std::uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpus[i], &props);
        auto reject = [&](const char* why) {
            rejected += std::format("{}{}: {}", rejected.empty() ? "" : "; ", props.deviceName, why);
        };
        if (props.apiVersion < VK_API_VERSION_1_1) {
            reject("Vulkan 1.1 not supported");
            continue;
        }
        std::uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(gpus[i], nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(gpus[i], nullptr, &extCount, exts.data());
        if (!std::ranges::any_of(exts, [](const VkExtensionProperties& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            })) {
            reject("no swapchain support");
            continue;
        }
        std::uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qCount, qs.data());
        std::uint32_t graphics = UINT32_MAX, present = UINT32_MAX;
        for (std::uint32_t q = 0; q < qCount; ++q) {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, m_surface, &canPresent);
            const bool gfx = (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (gfx && canPresent) {
                graphics = present = q;
                break;
            }
            if (gfx && graphics == UINT32_MAX)
                graphics = q;
            if (canPresent && present == UINT32_MAX)
                present = q;
        }
        if (graphics == UINT32_MAX || present == UINT32_MAX) {
            reject("cannot present to this window");
            continue;
        }
        int score = 0;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 200; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 1; break; // llvmpipe/lavapipe: last resort
        default: score = 10; break;
        }
        usable.push_back({gpus[i], graphics, present, score, static_cast<int>(i)});
    }
    if (usable.empty()) {
        *error = "no suitable Vulkan device (" + rejected + ")";
        return false;
    }
    const Candidate* pick = nullptr;
    for (const auto& c : usable)
        if (c.index == preferred)
            pick = &c;
    if (!pick)
        pick = &*std::ranges::max_element(usable, {}, &Candidate::score);
    m_gpu = pick->gpu;
    m_queueFamily = pick->graphics;
    m_presentFamily = pick->present;
    vkGetPhysicalDeviceProperties(m_gpu, &m_gpuProps);
    return true;
}

bool VulkanDevice::createLogicalDevice(std::string* error) {
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_gpu, &supported);
    VkPhysicalDeviceFeatures enabled{};
    m_anisotropySupported = supported.samplerAnisotropy == VK_TRUE;
    enabled.samplerAnisotropy = supported.samplerAnisotropy;

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    for (std::uint32_t family : {m_queueFamily, m_presentFamily}) {
        if (!queues.empty() && queues[0].queueFamilyIndex == family)
            continue;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queues.push_back(q);
    }

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    std::uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> avail(extCount);
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &extCount, avail.data());
    for (const auto& e : avail)
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
            exts.push_back("VK_KHR_portability_subset");

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
    ci.pQueueCreateInfos = queues.data();
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &enabled;
    VK_CHECK_INIT(vkCreateDevice(m_gpu, &ci, nullptr, &m_device));
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    vkGetDeviceQueue(m_device, m_presentFamily, 0, &m_presentQueue);

    VmaVulkanFunctions fns{};
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_1;
    ai.physicalDevice = m_gpu;
    ai.device = m_device;
    ai.instance = m_instance;
    VK_CHECK_INIT(vmaImportVulkanFunctionsFromVolk(&ai, &fns));
    ai.pVulkanFunctions = &fns;
    VK_CHECK_INIT(vmaCreateAllocator(&ai, &m_allocator));
    return true;
}

VkFormat VulkanDevice::pickDepthFormat() const {
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT,
                       VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(m_gpu, f, &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return f;
    }
    return VK_FORMAT_D16_UNORM;
}

bool VulkanDevice::createLayouts(std::string* error) {
    VkDescriptorSetLayoutBinding frame{};
    frame.binding = 0;
    frame.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    frame.descriptorCount = 1;
    frame.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &frame;
    VK_CHECK_INIT(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_frameSetLayout));

    VkDescriptorSetLayoutBinding tex{};
    tex.binding = 0;
    tex.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tex.descriptorCount = 1;
    tex.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.pBindings = &tex;
    VK_CHECK_INIT(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_texSetLayout));

    const VkDescriptorSetLayout sets[3] = {m_frameSetLayout, m_texSetLayout, m_texSetLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(detail::GpuDrawConstants)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 3;
    pi.pSetLayouts = sets;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &push;
    VK_CHECK_INIT(vkCreatePipelineLayout(m_device, &pi, nullptr, &m_pipelineLayout));

    auto module = [&](std::span<const std::uint8_t> spv, VkShaderModule& out) {
        const auto words = toWords(spv);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size();
        mi.pCode = words.data();
        return vkCreateShaderModule(m_device, &mi, nullptr, &out);
    };
    VK_CHECK_INIT(module(MM2_SHADER_SPV(mesh_vert), m_meshVs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(mesh_frag), m_meshFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(overlay_vert), m_overlayVs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(overlay_frag), m_overlayFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(composite_vert), m_compVs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(composite_frag), m_compFs));
    return true;
}

void VulkanDevice::loadPipelineCache() {
    std::vector<std::byte> data;
    if (!m_pipelineCachePath.empty()) {
        if (auto bytes = file::readBinary(m_pipelineCachePath)) {
            // Only reuse data written by this exact GPU/driver (header layout per spec).
            if (bytes->size() >= 32) {
                std::uint32_t header[4];
                std::memcpy(header, bytes->data(), sizeof(header));
                const bool match = header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                                   header[2] == m_gpuProps.vendorID && header[3] == m_gpuProps.deviceID &&
                                   std::memcmp(bytes->data() + 16, m_gpuProps.pipelineCacheUUID, VK_UUID_SIZE) == 0;
                if (match)
                    data = std::move(*bytes);
            }
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache);
    }
}

void VulkanDevice::savePipelineCache() {
    if (m_pipelineCachePath.empty() || !m_pipelineCache)
        return;
    std::size_t size = 0;
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
        return;
    std::vector<std::byte> data(size);
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) == VK_SUCCESS)
        file::writeAtomic(m_pipelineCachePath, std::span<const std::byte>(data.data(), size));
}

bool VulkanDevice::init(const DeviceCreateInfo& ci, std::string* error) {
    m_settings = ci.settings;
    m_settings.sanitize();
    m_pipelineCachePath = ci.pipelineCachePath;
    const char* env = std::getenv("OPENMM2_VK_VALIDATION");
    const bool validation = m_settings.validation || (env && *env && *env != '0');

    if (!createInstance(validation, error) || !pickPhysicalDevice(m_settings.gpu, error) ||
        !createLogicalDevice(error) || !createLayouts(error))
        return false;
    m_depthFormat = pickDepthFormat();
    loadPipelineCache();

    m_info.backend = Backend::Vulkan;
    m_info.apiVersion = std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(m_gpuProps.apiVersion),
                                    VK_API_VERSION_MINOR(m_gpuProps.apiVersion), VK_API_VERSION_PATCH(m_gpuProps.apiVersion));
    m_info.deviceName = m_gpuProps.deviceName;
    m_info.driverInfo = std::format("driver 0x{:x}, vendor 0x{:04x}", m_gpuProps.driverVersion, m_gpuProps.vendorID);
    m_info.maxAnisotropy = m_anisotropySupported ? m_gpuProps.limits.maxSamplerAnisotropy : 1.0f;
    m_info.maxTextureSize = m_gpuProps.limits.maxImageDimension2D;
    const VkSampleCountFlags sc =
        m_gpuProps.limits.framebufferColorSampleCounts & m_gpuProps.limits.framebufferDepthSampleCounts;
    for (std::uint32_t s = 1; s <= 8; s *= 2)
        if (sc & s)
            m_info.msaaSamples.push_back(s);

    // Per-frame objects.
    for (Frame& f : m_frames) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = m_queueFamily;
        VK_CHECK_INIT(vkCreateCommandPool(m_device, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK_INIT(vkAllocateCommandBuffers(m_device, &cai, &f.cmd));
        VK_CHECK_INIT(vkAllocateCommandBuffers(m_device, &cai, &f.upload));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK_INIT(vkCreateFence(m_device, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK_INIT(vkCreateSemaphore(m_device, &sci, nullptr, &f.imageAvailable));
        f.geometry.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        f.uniforms.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        f.staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    VkCommandPoolCreateInfo lpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    lpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    lpci.queueFamilyIndex = m_queueFamily;
    VK_CHECK_INIT(vkCreateCommandPool(m_device, &lpci, nullptr, &m_loadPool));
    VkCommandBufferAllocateInfo lcai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    lcai.commandPool = m_loadPool;
    lcai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    lcai.commandBufferCount = 1;
    VK_CHECK_INIT(vkAllocateCommandBuffers(m_device, &lcai, &m_loadCmd));
    VkFenceCreateInfo lfci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK_INIT(vkCreateFence(m_device, &lfci, nullptr, &m_loadFence));

    if (!createSwapchain()) {
        *error = "swapchain creation failed";
        return false;
    }
    createSceneRenderPass();
    if (!createSceneTargets()) {
        *error = "scene target creation failed";
        return false;
    }

    const std::uint32_t white = 0xFFFFFFFFu;
    TextureDesc wd;
    wd.debugName = "white";
    const TextureData wdata{&white, 0};
    m_white = createTexture(wd, std::span(&wdata, 1));
    flushLoadUploads();
    return true;
}

VulkanDevice::~VulkanDevice() {
    if (!m_device) {
        if (m_surface)
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        if (m_messenger)
            vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
        if (m_instance)
            vkDestroyInstance(m_instance, nullptr);
        return;
    }
    vkDeviceWaitIdle(m_device);
    flushLoadUploads();
    savePipelineCache();
    for (Frame& f : m_frames) {
        for (auto& d : f.deletions)
            d();
        f.deletions.clear();
        destroyRing(f.geometry);
        destroyRing(f.uniforms);
        destroyRing(f.staging);
        vkDestroyFence(m_device, f.fence, nullptr);
        vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
        vkDestroyCommandPool(m_device, f.pool, nullptr);
    }
    if (m_captureBuffer.buffer)
        vmaDestroyBuffer(m_allocator, m_captureBuffer.buffer, m_captureBuffer.alloc);
    vkDestroyFence(m_device, m_loadFence, nullptr);
    vkDestroyCommandPool(m_device, m_loadPool, nullptr);
    m_textures.forEach([&](std::uint32_t, VkTex& t) {
        vkDestroyImageView(m_device, t.view, nullptr);
        vmaDestroyImage(m_allocator, t.image, t.alloc);
    });
    m_buffers.forEach([&](std::uint32_t, VkBuf& b) { vmaDestroyBuffer(m_allocator, b.buffer, b.alloc); });
    destroyPipelines(false);
    destroySceneTargets();
    destroySwapchainResources();
    if (m_swapchain)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    for (auto& [k, s] : m_samplers)
        vkDestroySampler(m_device, s, nullptr);
    for (VkDescriptorPool p : m_pools)
        vkDestroyDescriptorPool(m_device, p, nullptr);
    if (m_sceneSetPool)
        vkDestroyDescriptorPool(m_device, m_sceneSetPool, nullptr);
    for (VkShaderModule m : {m_meshVs, m_meshFs, m_overlayVs, m_overlayFs, m_compVs, m_compFs})
        vkDestroyShaderModule(m_device, m, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_frameSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_texSetLayout, nullptr);
    if (m_scenePass)
        vkDestroyRenderPass(m_device, m_scenePass, nullptr);
    if (m_outputPass)
        vkDestroyRenderPass(m_device, m_outputPass, nullptr);
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_messenger)
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    vkDestroyInstance(m_instance, nullptr);
}

void VulkanDevice::setDebugName(VkObjectType type, std::uint64_t handle, const std::string& name) {
    if (!m_debugUtils || name.empty() || !vkSetDebugUtilsObjectNameEXT)
        return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name.c_str();
    vkSetDebugUtilsObjectNameEXT(m_device, &ni);
}

// ---------------------------------------------------------------------------------
// Swapchain and targets
// ---------------------------------------------------------------------------------

void VulkanDevice::createOutputRenderPass() {
    if (m_outputPass)
        vkDestroyRenderPass(m_device, m_outputPass, nullptr);
    VkAttachmentDescription color{};
    color.format = m_swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // Wait for the acquire semaphore (signalled at COLOR_ATTACHMENT_OUTPUT)
    // before the clear/load; make the resolved scene image readable.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &m_outputPass), "vkCreateRenderPass(output)");
}

void VulkanDevice::createSceneRenderPass() {
    if (m_scenePass)
        vkDestroyRenderPass(m_device, m_scenePass, nullptr);
    std::uint32_t samples = 1;
    for (std::uint32_t s : m_info.msaaSamples)
        if (s <= m_settings.msaa)
            samples = s;
    m_samples = static_cast<VkSampleCountFlagBits>(samples);
    const bool msaa = m_samples != VK_SAMPLE_COUNT_1_BIT;

    std::vector<VkAttachmentDescription> atts;
    VkAttachmentDescription color{};
    color.format = kSceneColorFormat;
    color.samples = m_samples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    atts.push_back(color);
    VkAttachmentDescription depth{};
    depth.format = m_depthFormat;
    depth.samples = m_samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    atts.push_back(depth);
    if (msaa) {
        VkAttachmentDescription resolve = color;
        resolve.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        atts.push_back(resolve);
    }
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    sub.pResolveAttachments = msaa ? &resolveRef : nullptr;

    // The scene images are shared by both frames in flight: wait for the
    // previous frame's composite read (fragment shader) and depth writes.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = static_cast<std::uint32_t>(atts.size());
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &m_scenePass), "vkCreateRenderPass(scene)");
}

bool VulkanDevice::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps) != VK_SUCCESS)
        return false;
    const platform::Extent px = m_window.pixelSize();
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(static_cast<std::uint32_t>(std::max(px.width, 1)), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<std::uint32_t>(std::max(px.height, 1)), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return false; // minimised

    std::uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &fmtCount, formats.data());
    if (formats.empty())
        return false;
    // Gamma-space (UNORM) output: the game's colours are already sRGB-encoded.
    VkSurfaceFormatKHR chosen = formats[0];
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32}) {
        auto it = std::ranges::find_if(formats, [&](const VkSurfaceFormatKHR& f) {
            return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) {
            chosen = *it;
            break;
        }
    }

    std::uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> modes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &pmCount, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::ranges::find(modes, m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    switch (m_settings.vsync) {
    case VsyncMode::Off:
        mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
               : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                  : VK_PRESENT_MODE_FIFO_KHR;
        break;
    case VsyncMode::Mailbox: mode = has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR; break;
    case VsyncMode::Adaptive:
        mode = has(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
        break;
    case VsyncMode::On: break;
    }

    std::uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount)
        imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    m_swapCanCopy = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (m_swapCanCopy)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const std::uint32_t families[2] = {m_queueFamily, m_presentFamily};
    if (m_queueFamily != m_presentFamily) {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = m_swapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (VkResult r = vkCreateSwapchainKHR(m_device, &ci, nullptr, &newSwapchain); r != VK_SUCCESS) {
        logVk(r, "vkCreateSwapchainKHR");
        return false;
    }
    destroySwapchainResources();
    if (m_swapchain)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = newSwapchain;

    const bool formatChanged = chosen.format != m_swapFormat;
    m_swapFormat = chosen.format;
    if (formatChanged || !m_outputPass) {
        // Pipelines used in the output pass depend on its format.
        destroyPipelines(false);
        createOutputRenderPass();
    }

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, images.data());
    m_swapImages.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SwapImage& s = m_swapImages[i];
        s.image = images[i];
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_swapFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vi, nullptr, &s.view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = m_outputPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &s.view;
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        vkCreateFramebuffer(m_device, &fi, nullptr, &s.framebuffer);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(m_device, &sci, nullptr, &s.renderFinished);
    }
    m_output = {extent.width, extent.height};
    log::debug("vulkan: swapchain {}x{} format {} mode {} images {}", extent.width, extent.height,
               static_cast<int>(m_swapFormat), static_cast<int>(mode), count);
    m_swapchainDirty = false;
    return true;
}

void VulkanDevice::destroySwapchainResources() {
    for (SwapImage& s : m_swapImages) {
        vkDestroyFramebuffer(m_device, s.framebuffer, nullptr);
        vkDestroyImageView(m_device, s.view, nullptr);
        vkDestroySemaphore(m_device, s.renderFinished, nullptr);
    }
    m_swapImages.clear();
}

bool VulkanDevice::createSceneTargets() {
    m_sceneExtent = scaledExtent(m_output, m_settings.renderScale);
    const bool msaa = m_samples != VK_SAMPLE_COUNT_1_BIT;
    auto makeImage = [&](VkTex& t, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                         VkImageAspectFlags aspect, const char* name) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {m_sceneExtent.width, m_sceneExtent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = samples;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_allocator, &ii, &ai, &t.image, &t.alloc, nullptr) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        t.width = m_sceneExtent.width;
        t.height = m_sceneExtent.height;
        setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(t.image), name);
        return vkCreateImageView(m_device, &vi, nullptr, &t.view) == VK_SUCCESS;
    };
    if (!makeImage(m_sceneColor, kSceneColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "scene color"))
        return false;
    if (msaa && !makeImage(m_sceneMsaa, kSceneColorFormat,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, m_samples,
                           VK_IMAGE_ASPECT_COLOR_BIT, "scene msaa"))
        return false;
    if (!makeImage(m_sceneDepth, m_depthFormat,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, m_samples,
                   VK_IMAGE_ASPECT_DEPTH_BIT, "scene depth"))
        return false;

    std::vector<VkImageView> views;
    if (msaa)
        views = {m_sceneMsaa.view, m_sceneDepth.view, m_sceneColor.view};
    else
        views = {m_sceneColor.view, m_sceneDepth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = m_scenePass;
    fi.attachmentCount = static_cast<std::uint32_t>(views.size());
    fi.pAttachments = views.data();
    fi.width = m_sceneExtent.width;
    fi.height = m_sceneExtent.height;
    fi.layers = 1;
    if (vkCreateFramebuffer(m_device, &fi, nullptr, &m_sceneFramebuffer) != VK_SUCCESS)
        return false;

    // Descriptor for sampling the resolved scene in the composite pass.
    if (!m_sceneSetPool) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        vkCreateDescriptorPool(m_device, &pi, nullptr, &m_sceneSetPool);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_sceneSetPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_texSetLayout;
    if (vkAllocateDescriptorSets(m_device, &ai, &m_sceneSet) != VK_SUCCESS)
        return false;
    const Filter f = m_sceneExtent == m_output ? Filter::Point : Filter::Bilinear;
    VkDescriptorImageInfo img{sampler({f, AddressMode::Clamp, AddressMode::Clamp}), m_sceneColor.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_sceneSet;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    log::debug("vulkan: scene target {}x{} ({}x MSAA)", m_sceneExtent.width, m_sceneExtent.height,
               static_cast<int>(m_samples));
    return true;
}

void VulkanDevice::destroySceneTargets() {
    if (m_sceneSet) {
        vkFreeDescriptorSets(m_device, m_sceneSetPool, 1, &m_sceneSet);
        m_sceneSet = VK_NULL_HANDLE;
    }
    if (m_sceneFramebuffer)
        vkDestroyFramebuffer(m_device, m_sceneFramebuffer, nullptr);
    m_sceneFramebuffer = VK_NULL_HANDLE;
    for (VkTex* t : {&m_sceneColor, &m_sceneMsaa, &m_sceneDepth}) {
        if (t->view)
            vkDestroyImageView(m_device, t->view, nullptr);
        if (t->image)
            vmaDestroyImage(m_allocator, t->image, t->alloc);
        *t = {};
    }
}

void VulkanDevice::recreateAll() {
    vkDeviceWaitIdle(m_device);
    destroySceneTargets();
    if (!createSwapchain())
        return;
    createSceneTargets();
}

void VulkanDevice::applySettings(const DisplaySettings& settings) {
    DisplaySettings s = settings;
    s.sanitize();
    const bool swap = s.vsync != m_settings.vsync;
    const bool msaa = s.msaa != m_settings.msaa;
    const bool scale = s.renderScale != m_settings.renderScale;
    const bool aniso = s.anisotropy != m_settings.anisotropy;
    m_settings = s;
    if (!(swap || msaa || scale || aniso))
        return;
    vkDeviceWaitIdle(m_device);
    if (aniso) {
        // Every cached texture descriptor references a sampler; rebuild all.
        for (auto& [k, cs] : m_texSets)
            vkFreeDescriptorSets(m_device, cs.pool, 1, &cs.set);
        m_texSets.clear();
        destroySceneTargets();
        for (auto& [k, smp] : m_samplers)
            vkDestroySampler(m_device, smp, nullptr);
        m_samplers.clear();
        createSceneTargets();
    }
    if (msaa) {
        destroySceneTargets();
        destroyPipelines(true);
        createSceneRenderPass();
        createSceneTargets();
    }
    if (swap)
        m_swapchainDirty = true;
    if (scale && !msaa) {
        destroySceneTargets();
        createSceneTargets();
    }
}

// ---------------------------------------------------------------------------------
// Pipelines, samplers, descriptors
// ---------------------------------------------------------------------------------

void VulkanDevice::destroyPipelines(bool sceneOnly) {
    for (auto it = m_pipelines.begin(); it != m_pipelines.end();) {
        const bool isScene = (it->first >> 32) == static_cast<std::uint64_t>(Pass::Scene);
        if (!sceneOnly || isScene) {
            vkDestroyPipeline(m_device, it->second, nullptr);
            it = m_pipelines.erase(it);
        } else {
            ++it;
        }
    }
    if (!sceneOnly && m_compositePipeline) {
        vkDestroyPipeline(m_device, m_compositePipeline, nullptr);
        m_compositePipeline = VK_NULL_HANDLE;
    }
}

VkPipeline VulkanDevice::pipeline(const PipelineState& s, Pass pass) {
    const std::uint64_t key = (static_cast<std::uint64_t>(pass) << 32) | s.key();
    if (auto it = m_pipelines.find(key); it != m_pipelines.end())
        return it->second;

    const bool mesh = s.vertexFormat == VertexFormat::Mesh;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mesh ? m_meshVs : m_overlayVs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mesh ? m_meshFs : m_overlayFs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, mesh ? static_cast<std::uint32_t>(sizeof(Vertex3D))
                                                    : static_cast<std::uint32_t>(sizeof(Vertex2D)),
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attrs;
    if (mesh) {
        attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, position)},
                 {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, normal)},
                 {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex3D, color)},
                 {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex3D, uv0)},
                 {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex3D, uv1)}};
    } else {
        attrs = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex2D, position)},
                 {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex2D, uv)},
                 {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex2D, color)}};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = s.topology == Topology::TriangleList    ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                  : s.topology == Topology::TriangleStrip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                                          : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = s.cull == CullMode::None   ? VK_CULL_MODE_NONE
                  : s.cull == CullMode::Back ? VK_CULL_MODE_BACK_BIT
                                             : VK_CULL_MODE_FRONT_BIT;
    // The negative viewport height makes winding match OpenGL conventions.
    rs.frontFace = s.frontFace == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = s.depthBias ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = pass == Pass::Scene ? m_samples : VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const bool hasDepth = pass == Pass::Scene;
    ds.depthTestEnable = hasDepth && s.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = hasDepth && s.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = toVk(s.depthCompare);

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = s.colorWrite ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                                        : 0;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (s.blend) {
    case BlendMode::Opaque: blend.blendEnable = VK_FALSE; break;
    case BlendMode::Alpha:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::Modulate:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::Premultiplied:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Add:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (s.depthBias)
        dyn.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<std::uint32_t>(dyn.size());
    dy.pDynamicStates = dyn.data();

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = m_pipelineLayout;
    ci.renderPass = pass == Pass::Scene ? m_scenePass : m_outputPass;
    ci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    logVk(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
    m_pipelines.emplace(key, p);
    return p;
}

VkPipeline VulkanDevice::compositePipeline() {
    if (m_compositePipeline)
        return m_compositePipeline;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_compVs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_compFs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = m_pipelineLayout;
    ci.renderPass = m_outputPass;
    logVk(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_compositePipeline),
          "vkCreateGraphicsPipelines(composite)");
    return m_compositePipeline;
}

VkSampler VulkanDevice::sampler(const SamplerDesc& desc) {
    if (auto it = m_samplers.find(desc.key()); it != m_samplers.end())
        return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = desc.filter == Filter::Point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = ci.magFilter;
    ci.mipmapMode = desc.filter == Filter::Trilinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = toVk(desc.addressU);
    ci.addressModeV = toVk(desc.addressV);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    if (desc.filter == Filter::Trilinear && m_anisotropySupported && m_settings.anisotropy > 1) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(static_cast<float>(m_settings.anisotropy), m_gpuProps.limits.maxSamplerAnisotropy);
    }
    VkSampler s = VK_NULL_HANDLE;
    logVk(vkCreateSampler(m_device, &ci, nullptr, &s), "vkCreateSampler");
    m_samplers.emplace(desc.key(), s);
    return s;
}

VkDescriptorSet VulkanDevice::allocateSet(VkDescriptorSetLayout layout, VkDescriptorPool* poolOut) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!m_pools.empty()) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = m_pools.back();
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VkResult r = vkAllocateDescriptorSets(m_device, &ai, &set);
            if (r == VK_SUCCESS) {
                *poolOut = m_pools.back();
                return set;
            }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
                logVk(r, "vkAllocateDescriptorSets");
                return VK_NULL_HANDLE;
            }
        }
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
                                               {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 4096;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        logVk(vkCreateDescriptorPool(m_device, &pi, nullptr, &pool), "vkCreateDescriptorPool");
        m_pools.push_back(pool);
    }
    return VK_NULL_HANDLE;
}

VkDescriptorSet VulkanDevice::textureSet(std::uint32_t textureId, VkImageView view, const SamplerDesc& desc) {
    const std::uint64_t key = (static_cast<std::uint64_t>(textureId) << 32) | desc.key();
    if (auto it = m_texSets.find(key); it != m_texSets.end())
        return it->second.set;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = allocateSet(m_texSetLayout, &pool);
    if (!set)
        return VK_NULL_HANDLE;
    VkDescriptorImageInfo img{sampler(desc), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    m_texSets.emplace(key, CachedSet{set, pool});
    return set;
}

void VulkanDevice::forgetTextureSets(std::uint32_t textureId) {
    for (auto it = m_texSets.begin(); it != m_texSets.end();) {
        if ((it->first >> 32) == textureId) {
            const CachedSet cs = it->second;
            deferDelete([this, cs] { vkFreeDescriptorSets(m_device, cs.pool, 1, &cs.set); });
            it = m_texSets.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------------
// Memory: rings, staging, uploads
// ---------------------------------------------------------------------------------

std::pair<Ring::Chunk*, VkDeviceSize> VulkanDevice::ringAlloc(Ring& ring, VkDeviceSize size, VkDeviceSize align) {
    while (true) {
        if (ring.current >= ring.chunks.size()) {
            Ring::Chunk c;
            c.size = std::max(ring.usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT ? kStagingChunkSize : kRingChunkSize,
                              (size + align) * 2);
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = c.size;
            bi.usage = ring.usage;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuf b;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_allocator, &bi, &ai, &b.buffer, &b.alloc, &info) != VK_SUCCESS) {
                log::error("vulkan: out of memory for a {} byte ring chunk", c.size);
                return {nullptr, 0};
            }
            b.size = c.size;
            b.mapped = info.pMappedData;
            c.buffer = b.buffer;
            c.mapped = static_cast<std::uint8_t*>(info.pMappedData);
            c.handle = m_buffers.insert(b);
            if (ring.usage == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
                VkDescriptorPool pool;
                c.uboSet = allocateSet(m_frameSetLayout, &pool);
                VkDescriptorBufferInfo dbi{c.buffer, 0, sizeof(detail::GpuFrameConstants)};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = c.uboSet;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w.pBufferInfo = &dbi;
                vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
            }
            ring.chunks.push_back(c);
        }
        Ring::Chunk& c = ring.chunks[ring.current];
        const VkDeviceSize offset = (c.offset + align - 1) / align * align;
        if (offset + size <= c.size) {
            c.offset = offset + size;
            return {&c, offset};
        }
        ++ring.current;
    }
}

void VulkanDevice::resetRing(Ring& ring) {
    for (auto& c : ring.chunks)
        c.offset = 0;
    ring.current = 0;
}

void VulkanDevice::destroyRing(Ring& ring) {
    for (auto& c : ring.chunks)
        if (auto b = m_buffers.remove(c.handle))
            vmaDestroyBuffer(m_allocator, b->buffer, b->alloc);
    ring.chunks.clear();
    ring.current = 0;
}

void VulkanDevice::deferDelete(std::function<void()> fn) {
    if (m_inFrame)
        m_frames[m_frameIndex].deletions.push_back(std::move(fn));
    else
        m_frames[(m_frameIndex + kFramesInFlight - 1) % kFramesInFlight].deletions.push_back(std::move(fn));
}

VkCommandBuffer VulkanDevice::uploadCommands() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_inFrame) {
        Frame& f = m_frames[m_frameIndex];
        if (!f.uploadRecording) {
            vkBeginCommandBuffer(f.upload, &bi);
            f.uploadRecording = true;
        }
        return f.upload;
    }
    if (!m_loadRecording) {
        vkBeginCommandBuffer(m_loadCmd, &bi);
        m_loadRecording = true;
    }
    return m_loadCmd;
}

std::pair<VkBuffer, VkDeviceSize> VulkanDevice::stage(const void* data, VkDeviceSize size, VkDeviceSize align) {
    if (m_inFrame) {
        auto [chunk, offset] = ringAlloc(m_frames[m_frameIndex].staging, size, align);
        if (!chunk)
            return {VK_NULL_HANDLE, 0};
        std::memcpy(chunk->mapped + offset, data, size);
        return {chunk->buffer, offset};
    }
    // Loading: a dedicated staging buffer, freed after the batch is submitted.
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(size, 4);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuf b;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bi, &ai, &b.buffer, &b.alloc, &info) != VK_SUCCESS)
        return {VK_NULL_HANDLE, 0};
    std::memcpy(info.pMappedData, data, size);
    m_loadStaging.push_back(b);
    m_loadStagingBytes += size;
    return {b.buffer, 0};
}

void VulkanDevice::flushLoadUploads() {
    if (!m_loadRecording)
        return;
    vkEndCommandBuffer(m_loadCmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_loadCmd;
    logVk(vkQueueSubmit(m_queue, 1, &si, m_loadFence), "vkQueueSubmit(load)");
    vkWaitForFences(m_device, 1, &m_loadFence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_loadFence);
    vkResetCommandBuffer(m_loadCmd, 0);
    m_loadRecording = false;
    for (VkBuf& b : m_loadStaging)
        vmaDestroyBuffer(m_allocator, b.buffer, b.alloc);
    m_loadStaging.clear();
    m_loadStagingBytes = 0;
}

// ---------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc, std::span<const TextureData> mips) {
    VkTex t;
    t.width = std::max(desc.width, 1u);
    t.height = std::max(desc.height, 1u);
    t.mips = std::clamp(desc.mipLevels, 1u, mipCount(t.width, t.height));
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {t.width, t.height, 1};
    ii.mipLevels = t.mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (VkResult r = vmaCreateImage(m_allocator, &ii, &ai, &t.image, &t.alloc, nullptr); r != VK_SUCCESS) {
        logVk(r, "vmaCreateImage");
        return {};
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mips, 0, 1};
    vkCreateImageView(m_device, &vi, nullptr, &t.view);
    setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(t.image), desc.debugName);

    VkCommandBuffer cmd = uploadCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mips, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    for (std::uint32_t level = 0; level < t.mips && level < mips.size(); ++level) {
        const TextureData& d = mips[level];
        if (!d.data)
            continue;
        const std::uint32_t w = std::max(1u, t.width >> level), h = std::max(1u, t.height >> level);
        const std::uint32_t pitch = d.rowPitch ? d.rowPitch : w * 4;
        // Repack to tight rows so the copy needs no row-length tricks.
        std::vector<std::uint8_t> tight;
        const void* src = d.data;
        if (pitch != w * 4) {
            tight.resize(static_cast<std::size_t>(w) * h * 4);
            for (std::uint32_t y = 0; y < h; ++y)
                std::memcpy(&tight[static_cast<std::size_t>(y) * w * 4], static_cast<const std::uint8_t*>(d.data) + static_cast<std::size_t>(y) * pitch, w * 4);
            src = tight.data();
        }
        auto [buf, off] = stage(src, static_cast<VkDeviceSize>(w) * h * 4, 16);
        if (!buf)
            continue;
        VkBufferImageCopy copy{};
        copy.bufferOffset = off;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, buf, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    if (!m_inFrame && m_loadStagingBytes > kLoadFlushThreshold)
        flushLoadUploads();
    return TextureHandle{m_textures.insert(t)};
}

void VulkanDevice::updateTexture(TextureHandle handle, std::uint32_t mip, const Rect& r, const void* data,
                                 std::uint32_t rowPitch) {
    const VkTex* t = m_textures.get(handle.id);
    if (!t || mip >= t->mips || !data || r.width == 0 || r.height == 0)
        return;
    const std::uint32_t pitch = rowPitch ? rowPitch : r.width * 4;
    std::vector<std::uint8_t> tight(static_cast<std::size_t>(r.width) * r.height * 4);
    for (std::uint32_t y = 0; y < r.height; ++y)
        std::memcpy(&tight[static_cast<std::size_t>(y) * r.width * 4],
                    static_cast<const std::uint8_t*>(data) + static_cast<std::size_t>(y) * pitch, r.width * 4);
    auto [buf, off] = stage(tight.data(), tight.size(), 16);
    if (!buf)
        return;
    VkCommandBuffer cmd = uploadCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0; // write-after-read: execution dependency suffices
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.bufferOffset = off;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    copy.imageOffset = {r.x, r.y, 0};
    copy.imageExtent = {r.width, r.height, 1};
    vkCmdCopyBufferToImage(cmd, buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

void VulkanDevice::destroyTexture(TextureHandle handle) {
    if (handle == m_white)
        return;
    auto t = m_textures.remove(handle.id);
    if (!t)
        return;
    forgetTextureSets(handle.id);
    VkTex tex = *t;
    for (auto& s : m_boundTexSet)
        s = VK_NULL_HANDLE;
    deferDelete([this, tex] {
        vkDestroyImageView(m_device, tex.view, nullptr);
        vmaDestroyImage(m_allocator, tex.image, tex.alloc);
    });
}

BufferHandle VulkanDevice::createBuffer(BufferKind kind, std::size_t size, const void* data) {
    VkBuf b;
    b.size = std::max<std::size_t>(size, 4);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = b.size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               (kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (VkResult r = vmaCreateBuffer(m_allocator, &bi, &ai, &b.buffer, &b.alloc, nullptr); r != VK_SUCCESS) {
        logVk(r, "vmaCreateBuffer");
        return {};
    }
    const BufferHandle h{m_buffers.insert(b)};
    if (data)
        updateBuffer(h, 0, std::span(static_cast<const std::byte*>(data), size));
    return h;
}

void VulkanDevice::updateBuffer(BufferHandle handle, std::size_t offset, std::span<const std::byte> data) {
    // Copy the entry: stage() may grow a staging ring, which inserts into
    // m_buffers and would invalidate a pointer into it.
    const VkBuf* entry = m_buffers.get(handle.id);
    if (!entry || data.empty() || offset + data.size() > entry->size)
        return;
    const VkBuf buf = *entry;
    const VkBuf* b = &buf;
    auto [src, srcOff] = stage(data.data(), data.size(), 16);
    if (!src)
        return;
    VkCommandBuffer cmd = uploadCommands();
    VkBufferMemoryBarrier before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = b->buffer;
    before.offset = offset;
    before.size = data.size();
    before.srcAccessMask = 0;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &before, 0, nullptr);
    VkBufferCopy copy{srcOff, offset, data.size()};
    vkCmdCopyBuffer(cmd, src, b->buffer, 1, &copy);
    VkBufferMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1,
                         &after, 0, nullptr);
    if (!m_inFrame && m_loadStagingBytes > kLoadFlushThreshold)
        flushLoadUploads();
}

void VulkanDevice::destroyBuffer(BufferHandle handle) {
    auto b = m_buffers.remove(handle.id);
    if (!b)
        return;
    VkBuf buf = *b;
    if (m_boundVb == buf.buffer)
        m_boundVb = VK_NULL_HANDLE;
    if (m_boundIb == buf.buffer)
        m_boundIb = VK_NULL_HANDLE;
    deferDelete([this, buf] { vmaDestroyBuffer(m_allocator, buf.buffer, buf.alloc); });
}

BufferSlice VulkanDevice::uploadTransient(BufferKind, std::span<const std::byte> data) {
    if (!m_inFrame)
        return {};
    auto [chunk, offset] = ringAlloc(m_frames[m_frameIndex].geometry, std::max<std::size_t>(data.size(), 4), 16);
    if (!chunk)
        return {};
    std::memcpy(chunk->mapped + offset, data.data(), data.size());
    m_stats.transientBytes += data.size();
    return {BufferHandle{chunk->handle}, static_cast<std::uint32_t>(offset)};
}

// ---------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------

bool VulkanDevice::beginFrame() {
    if (m_window.minimized() || m_window.pixelSize().empty())
        return false;
    flushLoadUploads();

    const platform::Extent px = m_window.pixelSize();
    if (m_resizeHint) {
        m_resizeHint = false;
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps) == VK_SUCCESS &&
            caps.currentExtent.width != UINT32_MAX &&
            (caps.currentExtent.width != m_output.width || caps.currentExtent.height != m_output.height))
            m_swapchainDirty = true;
    }
    if (m_swapchainDirty || !m_swapchain ||
        (static_cast<std::uint32_t>(px.width) != m_output.width || static_cast<std::uint32_t>(px.height) != m_output.height)) {
        log::debug("vulkan: recreating swapchain (dirty={}, window {}x{}, swapchain {}x{})", m_swapchainDirty, px.width,
                   px.height, m_output.width, m_output.height);
        recreateAll();
        if (!m_swapchain || m_swapchainDirty)
            return false;
    }

    Frame& f = m_frames[m_frameIndex];
    vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    for (auto& d : f.deletions)
        d();
    f.deletions.clear();

    VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_swapchainDirty = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        logVk(r, "vkAcquireNextImageKHR");
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        log::debug("vulkan: acquire reported a suboptimal swapchain");
        m_swapchainDirty = true; // recreate after this frame
    }

    vkResetCommandPool(m_device, f.pool, 0);
    f.uploadRecording = false;
    resetRing(f.geometry);
    resetRing(f.uniforms);
    resetRing(f.staging);
    f.captured = false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

    m_stats = {};
    m_stats.apiErrors = m_validationErrors;
    m_inFrame = true;
    m_sceneRendered = false;
    m_pass = Pass::None;
    return true;
}

void VulkanDevice::bindDefaultViewport() {
    const Extent2D e = m_pass == Pass::Scene ? m_sceneExtent : m_output;
    setViewport({0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1});
    setScissor(nullptr);
}

void VulkanDevice::beginScene(const ClearValues& cv) {
    Frame& f = m_frames[m_frameIndex];
    const bool msaa = m_samples != VK_SAMPLE_COUNT_1_BIT;
    VkClearValue clears[3]{};
    clears[0].color = {{cv.color.x, cv.color.y, cv.color.z, cv.color.w}};
    clears[1].depthStencil = {cv.depth, 0};
    clears[2].color = clears[0].color;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = m_scenePass;
    rb.framebuffer = m_sceneFramebuffer;
    rb.renderArea = {{0, 0}, {m_sceneExtent.width, m_sceneExtent.height}};
    rb.clearValueCount = msaa ? 3 : 2;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    m_pass = Pass::Scene;
    m_boundPipeline = VK_NULL_HANDLE;
    m_boundVb = m_boundIb = VK_NULL_HANDLE;
    m_boundTexSet[0] = m_boundTexSet[1] = VK_NULL_HANDLE;
    m_frameSetBound = false;
    bindDefaultViewport();
}

void VulkanDevice::endScene() {
    vkCmdEndRenderPass(m_frames[m_frameIndex].cmd);
    m_pass = Pass::None;
    m_sceneRendered = true;
}

void VulkanDevice::beginOverlay(const Vec4& clearColor) {
    Frame& f = m_frames[m_frameIndex];
    VkClearValue clear{};
    clear.color = {{clearColor.x, clearColor.y, clearColor.z, clearColor.w}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = m_outputPass;
    rb.framebuffer = m_swapImages[m_imageIndex].framebuffer;
    rb.renderArea = {{0, 0}, {m_output.width, m_output.height}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    m_pass = Pass::Overlay;
    m_boundPipeline = VK_NULL_HANDLE;
    m_boundVb = m_boundIb = VK_NULL_HANDLE;
    m_boundTexSet[0] = m_boundTexSet[1] = VK_NULL_HANDLE;
    m_frameSetBound = false;
    bindDefaultViewport();
    if (m_sceneRendered) {
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline());
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &m_sceneSet, 0, nullptr);
        // OpenMM2 image filters (composite.frag's push constants).
        const detail::GpuPostConstants pc = detail::toGpu(postProcess());
        vkCmdPushConstants(f.cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
        vkCmdDraw(f.cmd, 3, 1, 0, 0);
        m_boundPipeline = m_compositePipeline;
        m_boundTexSet[0] = m_sceneSet;
    }
}

void VulkanDevice::endOverlay() {
    vkCmdEndRenderPass(m_frames[m_frameIndex].cmd);
    m_pass = Pass::None;
}

void VulkanDevice::endFrame() {
    Frame& f = m_frames[m_frameIndex];
    const SwapImage& img = m_swapImages[m_imageIndex];

    if (m_captureRequested && m_swapCanCopy) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_output.width) * m_output.height * 4;
        if (!m_captureBuffer.buffer || m_captureBuffer.size < bytes) {
            if (m_captureBuffer.buffer) {
                vkDeviceWaitIdle(m_device);
                vmaDestroyBuffer(m_allocator, m_captureBuffer.buffer, m_captureBuffer.alloc);
            }
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            vmaCreateBuffer(m_allocator, &bi, &ai, &m_captureBuffer.buffer, &m_captureBuffer.alloc, &info);
            m_captureBuffer.size = bytes;
            m_captureBuffer.mapped = info.pMappedData;
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {m_output.width, m_output.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_captureBuffer.buffer, 1, &copy);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = m_captureBuffer.buffer;
        hb.size = VK_WHOLE_SIZE;
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb,
                             0, nullptr);
        m_captureExtent = m_output;
        m_captureFrame = m_frameIndex;
        f.captured = true;
        m_captureRequested = false;
    } else if (m_captureRequested) {
        log::warn("vulkan: swapchain does not support readback; capture skipped");
        m_captureRequested = false;
    }

    vkEndCommandBuffer(f.cmd);
    VkCommandBuffer cmds[2];
    std::uint32_t cmdCount = 0;
    if (f.uploadRecording) {
        vkEndCommandBuffer(f.upload);
        cmds[cmdCount++] = f.upload;
    }
    cmds[cmdCount++] = f.cmd;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = cmdCount;
    si.pCommandBuffers = cmds;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &img.renderFinished;
    vkResetFences(m_device, 1, &f.fence);
    logVk(vkQueueSubmit(m_queue, 1, &si, f.fence), "vkQueueSubmit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &img.renderFinished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_imageIndex;
    const VkResult r = vkQueuePresentKHR(m_presentQueue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        log::debug("vulkan: present reported {}", vkResultName(r));
        m_swapchainDirty = true;
    } else if (r != VK_SUCCESS)
        logVk(r, "vkQueuePresentKHR");

    if (f.captured)
        m_captureReady = true;
    m_inFrame = false;
    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
    m_stats.apiErrors = m_validationErrors;
}

bool VulkanDevice::readCapture(Image& out) {
    if (!m_captureReady)
        return false;
    vkWaitForFences(m_device, 1, &m_frames[m_captureFrame].fence, VK_TRUE, UINT64_MAX);
    vmaInvalidateAllocation(m_allocator, m_captureBuffer.alloc, 0, VK_WHOLE_SIZE);
    out.width = m_captureExtent.width;
    out.height = m_captureExtent.height;
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    std::memcpy(out.pixels.data(), m_captureBuffer.mapped, out.pixels.size());
    const bool bgra = m_swapFormat == VK_FORMAT_B8G8R8A8_UNORM || m_swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (std::size_t i = 0; i < out.pixels.size(); i += 4) {
        if (bgra)
            std::swap(out.pixels[i], out.pixels[i + 2]);
        out.pixels[i + 3] = 255;
    }
    m_captureReady = false;
    return true;
}

void VulkanDevice::waitIdle() {
    flushLoadUploads();
    vkDeviceWaitIdle(m_device);
}

// ---------------------------------------------------------------------------------
// Pass commands
// ---------------------------------------------------------------------------------

void VulkanDevice::setViewport(const Viewport& v) {
    m_viewport = v;
    // Negative height flips Vulkan's y-down clip space to the engine's y-up.
    VkViewport vp{v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth};
    vkCmdSetViewport(m_frames[m_frameIndex].cmd, 0, 1, &vp);
}

void VulkanDevice::setScissor(const Rect* r) {
    const Extent2D e = m_pass == Pass::Scene ? m_sceneExtent : m_output;
    VkRect2D s{{0, 0}, {e.width, e.height}};
    if (r) {
        const std::int32_t x0 = std::clamp(r->x, 0, static_cast<std::int32_t>(e.width));
        const std::int32_t y0 = std::clamp(r->y, 0, static_cast<std::int32_t>(e.height));
        const std::int32_t x1 = std::clamp(r->x + static_cast<std::int32_t>(r->width), x0, static_cast<std::int32_t>(e.width));
        const std::int32_t y1 = std::clamp(r->y + static_cast<std::int32_t>(r->height), y0, static_cast<std::int32_t>(e.height));
        s = {{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    }
    vkCmdSetScissor(m_frames[m_frameIndex].cmd, 0, 1, &s);
}

void VulkanDevice::clear(const ClearValues& cv) {
    if (m_pass == Pass::None)
        return;
    const Extent2D e = m_pass == Pass::Scene ? m_sceneExtent : m_output;
    VkClearAttachment atts[2]{};
    std::uint32_t n = 0;
    if (cv.clearColor) {
        atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        atts[n].colorAttachment = 0;
        atts[n].clearValue.color = {{cv.color.x, cv.color.y, cv.color.z, cv.color.w}};
        ++n;
    }
    if (cv.clearDepth && m_pass == Pass::Scene) {
        atts[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        atts[n].clearValue.depthStencil = {cv.depth, 0};
        ++n;
    }
    if (!n)
        return;
    const std::int32_t x0 = std::clamp(static_cast<std::int32_t>(m_viewport.x), 0, static_cast<std::int32_t>(e.width));
    const std::int32_t y0 = std::clamp(static_cast<std::int32_t>(m_viewport.y), 0, static_cast<std::int32_t>(e.height));
    const std::int32_t x1 = std::clamp(static_cast<std::int32_t>(m_viewport.x + m_viewport.width), x0,
                                       static_cast<std::int32_t>(e.width));
    const std::int32_t y1 = std::clamp(static_cast<std::int32_t>(m_viewport.y + m_viewport.height), y0,
                                       static_cast<std::int32_t>(e.height));
    if (x1 <= x0 || y1 <= y0)
        return;
    VkClearRect rect{{{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].cmd, n, atts, 1, &rect);
}

void VulkanDevice::setFrameConstants(const FrameConstants& fc) {
    if (!m_inFrame)
        return;
    const detail::GpuFrameConstants g = detail::toGpu(fc);
    Frame& f = m_frames[m_frameIndex];
    auto [chunk, offset] = ringAlloc(f.uniforms, sizeof(g), m_gpuProps.limits.minUniformBufferOffsetAlignment);
    if (!chunk)
        return;
    std::memcpy(chunk->mapped + offset, &g, sizeof(g));
    const std::uint32_t dyn = static_cast<std::uint32_t>(offset);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &chunk->uboSet, 1, &dyn);
    m_frameSetBound = true;
}

void VulkanDevice::draw(const DrawCall& call) {
    if (call.count == 0 || m_pass == Pass::None)
        return;
    const VkBuf* vb = m_buffers.get(call.vertices.buffer.id);
    if (!vb)
        return;
    const VkBuf* ib = call.indices ? m_buffers.get(call.indices.buffer.id) : nullptr;
    if (call.indices && !ib)
        return;
    VkCommandBuffer cmd = m_frames[m_frameIndex].cmd;

    if (!m_frameSetBound)
        setFrameConstants(FrameConstants{});

    const VkPipeline p = pipeline(effectiveState(call.state), m_pass);
    if (!p)
        return;
    if (p != m_boundPipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        m_boundPipeline = p;
        ++m_stats.pipelineBinds;
    }
    if (call.state.depthBias)
        vkCmdSetDepthBias(cmd, -call.depthBias, 0.0f, -call.depthBiasSlope);

    if (vb->buffer != m_boundVb || call.vertices.offset != m_boundVbOffset) {
        const VkDeviceSize off = call.vertices.offset;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &off);
        m_boundVb = vb->buffer;
        m_boundVbOffset = off;
    }
    const VkIndexType itype = call.indexType == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (ib && (ib->buffer != m_boundIb || call.indices.offset != m_boundIbOffset || itype != m_boundIbType)) {
        vkCmdBindIndexBuffer(cmd, ib->buffer, call.indices.offset, itype);
        m_boundIb = ib->buffer;
        m_boundIbOffset = call.indices.offset;
        m_boundIbType = itype;
    }

    for (std::uint32_t stage = 0; stage < 2; ++stage) {
        const TextureBinding& tb = call.textures[stage];
        const VkTex* t = m_textures.get(tb.texture.id);
        const std::uint32_t id = t ? tb.texture.id : m_white.id;
        if (!t)
            t = m_textures.get(m_white.id);
        const VkDescriptorSet set = textureSet(id, t->view, tb.sampler);
        if (set != m_boundTexSet[stage]) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1 + stage, 1, &set, 0, nullptr);
            m_boundTexSet[stage] = set;
        }
    }

    const detail::GpuDrawConstants dc = detail::toGpu(call.constants);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dc), &dc);

    if (ib)
        vkCmdDrawIndexed(cmd, call.count, 1, call.first, call.baseVertex, 0);
    else
        vkCmdDraw(cmd, call.count, 1, call.first, 0);
    ++m_stats.drawCalls;
    const Topology top = call.state.topology;
    m_stats.primitives += top == Topology::LineList        ? call.count / 2
                          : top == Topology::TriangleStrip ? (call.count > 2 ? call.count - 2 : 0)
                                                           : call.count / 3;
}

} // namespace

std::unique_ptr<Device> createVulkanDevice(const DeviceCreateInfo& info, std::string* error) {
    if (!info.window || info.window->api() != platform::GraphicsApi::Vulkan) {
        *error = "window was not created for Vulkan";
        return nullptr;
    }
    auto dev = std::make_unique<VulkanDevice>(*info.window);
    if (!dev->init(info, error))
        return nullptr;
    return dev;
}

} // namespace mm2::render
