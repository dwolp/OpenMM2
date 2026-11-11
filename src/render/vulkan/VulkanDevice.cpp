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

#include "core/Trace.h"
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

#include "render/vulkan/RayScene.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <deque>
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
// OpenMM2's bloom chain: half floats, so its wide, faint glows do not band
// (half resolution and below, so cheap; full-size RGBA16F post images cost
// ~0.7 ms at 1080p on an RTX 4070 and stay 8-bit, the composite dithers).
constexpr VkFormat kBloomColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

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
    VkDeviceAddress address = 0; // with ray tracing: for the acceleration structure builds
    bool transient = false;      // a ring chunk (per-frame memory)
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
    bool beginShadowPass(std::uint32_t size, bool keep) override;
    void endShadowPass() override;
    bool shadowMapKept() const override { return m_shadowKept; }
    bool beginProbePass(std::uint32_t width, std::uint32_t height, bool keep) override;
    void endProbePass() override;
    bool probeKept() const override { return m_probeKept; }
    bool rayTracingSupported() const override { return m_rayTracing; }
    bool beginRayScene(bool statics, bool first, bool last) override;
    bool rayStatics() const override { return m_pass == Pass::RayScene && m_rayScene.capturingStatics(); }
    bool rayStaticsWhole() const override { return !m_rayScene.capturingPart(); }
    void setRayDynamic() override {
        if (m_pass == Pass::RayScene)
            m_rayScene.setLayer(vk::RayScene::Layer::Dynamic);
    }
    void endRayScene() override;
    TextureHandle probeTexture() const override {
        return m_probeThisFrame && m_pass != Pass::Probe ? TextureHandle{m_probeId} : TextureHandle{};
    }
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
    enum class Pass { None, Scene, Overlay, Shadow, Probe, RayScene };

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
    // OpenMM2 post-processing chain (bloom): offscreen images at fractions
    // of the scene target, drawn with full-screen passes between the scene
    // and the output pass.
    VkRenderPass createPostRenderPass(VkFormat format);
    bool createPostTargets();
    void destroyPostTargets();
    VkPipeline fullscreenPipeline(VkShaderModule fs, VkRenderPass pass, const char* name);
    struct PostTarget;
    // One full-screen pass into `dst` with textures `set1` / `set2` (either
    // may be null) and `size` bytes of push constants.
    void drawPostPass(VkCommandBuffer cmd, const PostTarget& dst, VkPipeline pipeline, VkDescriptorSet set1,
                      VkDescriptorSet set2, const void* constants, std::uint32_t size,
                      VkDescriptorSet set3 = VK_NULL_HANDLE);
    void runAo(VkCommandBuffer cmd);
    void runBloom(VkCommandBuffer cmd);
    // OpenMM2 sun shadows: the shadow map and its render pass.
    void createShadowRenderPass();
    bool ensureShadowMap(std::uint32_t size);
    void destroyShadowMap();
    // Set 3 for mesh draws: the shadow map in a scene pass after this
    // frame's shadow pass, the white texture otherwise.
    void bindShadowSet(VkCommandBuffer cmd);
    // OpenMM2 live reflections: the probe's render pass and targets.
    void createProbeRenderPass();
    bool ensureProbe(std::uint32_t width, std::uint32_t height);
    void destroyProbe();
    // OpenMM2 ray traced shadows: a draw kept as ray scene geometry, and the
    // frame's top level structure bound for the mesh shaders (set 4).
    void captureRay(const DrawCall& call);
    void bindRaySet(VkCommandBuffer cmd);
    VkDeviceAddress bufferAddress(VkBuffer buffer) const;
    Extent2D passExtent() const;
    // The scene image the post passes and the composite read: the shaded
    // one with ambient occlusion this frame.
    VkDescriptorSet postSourceSet() const;
    const VkTex& postSourceTex() const;
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
    std::uint32_t m_instanceVersion = VK_API_VERSION_1_1;
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

    // Post-processing chain (createPostTargets)
    struct PostTarget {
        VkTex tex;
        VkRenderPass pass = VK_NULL_HANDLE; // the post render pass of its format
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE; // sampling it, bilinear clamped
    };
    VkRenderPass m_postPass = VK_NULL_HANDLE; // RGBA8 (kSceneColorFormat)
    VkRenderPass m_postPassBloom = VK_NULL_HANDLE; // RGBA16F (kBloomColorFormat)
    VkRenderPass m_postPassR32 = VK_NULL_HANDLE, m_postPassRG16 = VK_NULL_HANDLE;
    // Ambient occlusion: distances, occlusion and distance, the shaded scene.
    PostTarget m_aoDepth, m_aoRaw, m_aoScene;
    PostTarget m_ssrScene;   // wet roads: the scene with its reflections
    PostTarget m_shaftScene; // the scene with sun shafts
    PostTarget m_blurScene;  // the scene with motion blur
    // Temporal anti-aliasing: the output and the history at the window's
    // size, written in turn (m_taaIndex the next), valid after a frame.
    std::array<PostTarget, 2> m_taa{};
    std::uint32_t m_taaIndex = 0;
    bool m_taaValid = false;
    // Motion blur: the previous frame's scene camera, projection and time.
    std::optional<Mat44> m_prevProjection;
    std::optional<Mat34> m_prevCamera;
    float m_prevTime = 0.0f;
    const PostTarget* m_postCurrent = nullptr; // the last post image written this frame (null: the scene)
    VkDescriptorSet m_sceneDepthSet = VK_NULL_HANDLE; // the scene depth, point sampled
    bool m_depthSampleable = false;
    VkShaderModule m_depthCopyFs = VK_NULL_HANDLE, m_depthCopyMsFs = VK_NULL_HANDLE;
    VkShaderModule m_aoFs = VK_NULL_HANDLE, m_aoBlurFs = VK_NULL_HANDLE, m_ssrFs = VK_NULL_HANDLE;
    VkPipeline m_ssrPipeline = VK_NULL_HANDLE, m_shaftsPipeline = VK_NULL_HANDLE;
    VkShaderModule m_shaftsFs = VK_NULL_HANDLE, m_blurFs = VK_NULL_HANDLE, m_taaFs = VK_NULL_HANDLE;
    VkPipeline m_blurPipeline = VK_NULL_HANDLE;
    VkPipeline m_taaPipeline = VK_NULL_HANDLE;
    bool m_ssrThisFrame = false; // m_ssrScene holds this frame's wet scene
    VkPipeline m_depthCopyPipeline = VK_NULL_HANDLE, m_aoPipeline = VK_NULL_HANDLE, m_aoBlurPipeline = VK_NULL_HANDLE;
    bool m_aoThisFrame = false; // m_aoScene holds this frame's shaded scene

    // Sun shadows (beginShadowPass)
    VkRenderPass m_shadowPass = VK_NULL_HANDLE;
    VkRenderPass m_shadowPassKeep = VK_NULL_HANDLE; // loads the map instead of clearing it
    bool m_shadowDrawn = false;                     // the map holds an earlier frame's shadows
    bool m_shadowKept = false;                      // the last shadow pass kept them
    VkTex m_shadowTex;
    VkFramebuffer m_shadowFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet m_shadowSet = VK_NULL_HANDLE; // the map, point sampled
    std::uint32_t m_shadowSize = 0;
    bool m_shadowThisFrame = false; // the map holds this frame's shadows
    VkDescriptorSet m_boundShadowSet = VK_NULL_HANDLE;
    // Live reflections (beginProbePass): the colour image is a texture of
    // m_textures (m_probeId), so draws sample it like any other.
    VkRenderPass m_probePass = VK_NULL_HANDLE;
    VkRenderPass m_probePassKeep = VK_NULL_HANDLE; // loads the colour instead of clearing it
    // OpenMM2 ray traced shadows (VK_KHR_ray_query; m_rayTracing when the
    // GPU, its driver and the instance have it).
    bool m_rayTracing = false;
    vk::RayScene m_rayScene;
    VkDescriptorSetLayout m_raySetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_rayPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_raySets{};
    std::array<VkAccelerationStructureKHR, kFramesInFlight> m_rayWritten{}; // what each set names
    std::array<VkBuffer, kFramesInFlight> m_rayTableWritten{};
    // The alpha tested casters' textures, by slot in binding 2 (slot 0 is
    // white), and how many of them each frame's set has.
    static constexpr std::uint32_t kRayTextures = 2048;
    // Whether the rays' front faces wind the other way from the raster's
    // counter-clockwise ones (chosen by testing: reflections must show
    // the fronts).
    static constexpr bool kRayFlipFacing = false;
    struct RayTexture {
        std::uint32_t id = 0;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    std::vector<RayTexture> m_rayTextures;
    std::unordered_map<std::uint64_t, std::uint32_t> m_rayTextureSlots; // texture id << 32 | sampler key
    // Slots of destroyed textures, free again once the kept static layer
    // (captured anew within 600 frames) can no longer name them: they ran
    // out over a session (every time of day or weather reloads the city's
    // textures), and every texture after that read as white (the sky in
    // the wet roads' reflections).
    struct FreedRaySlot {
        std::uint32_t slot;
        std::uint64_t freeAt; // m_rayFrames (frames begun)
    };
    std::deque<FreedRaySlot> m_rayFreedSlots;
    std::vector<std::uint32_t> m_rayFreeSlots;
    std::uint64_t m_rayFrames = 0;
    std::array<std::uint32_t, kFramesInFlight> m_rayTexturesWritten{};
    std::array<std::vector<std::uint32_t>, kFramesInFlight> m_rayTexturesDirty{}; // slots to write again
    std::uint32_t rayTextureSlot(const TextureBinding& binding);
    VkAccelerationStructureKHR m_rayTop = VK_NULL_HANDLE; // this frame's (null: none built)
    VkShaderModule m_meshFsRay = VK_NULL_HANDLE;
    std::uint32_t m_probeId = 0;
    bool m_probeDrawn = false; // the image holds an earlier frame's probe
    bool m_probeKept = false;  // the last probe pass kept it
    VkTex m_probeDepth;
    VkFramebuffer m_probeFramebuffer = VK_NULL_HANDLE;
    Extent2D m_probeExtent;
    bool m_probeThisFrame = false; // the image holds this frame's probe
    VkDescriptorPool m_postSetPool = VK_NULL_HANDLE;
    std::array<PostTarget, detail::kBloomLevels> m_bloomDown;
    std::array<PostTarget, detail::kBloomLevels - 1> m_bloomUp;
    VkShaderModule m_bloomFs = VK_NULL_HANDLE;
    VkPipeline m_bloomPipeline = VK_NULL_HANDLE;
    bool m_bloomThisFrame = false; // m_bloomUp[0] holds this frame's glow

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
    // Vulkan 1.2 where the loader has it (OpenMM2 ray traced shadows use its
    // buffer device addresses); the device may still be 1.1.
    std::uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&instanceVersion);
    m_instanceVersion = instanceVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    app.apiVersion = m_instanceVersion;

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
    // mesh.vert writes gl_ClipDistance (OpenMM2 grass shells); every desktop
    // GPU has it.
    enabled.shaderClipDistance = supported.shaderClipDistance;

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

    // OpenMM2 ray traced shadows: ray queries in the fragment shaders over
    // acceleration structures, which need buffer device addresses.
    auto has = [&](const char* name) {
        return std::ranges::any_of(avail, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    if (m_instanceVersion >= VK_API_VERSION_1_2 && m_gpuProps.apiVersion >= VK_API_VERSION_1_2 &&
        has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
        has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) && !std::getenv("OPENMM2_NO_RAYTRACING")) {
        v12.pNext = &asf;
        asf.pNext = &rqf;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &v12;
        vkGetPhysicalDeviceFeatures2(m_gpu, &f2);
        // The alpha tested casters: their vertices read by address (64-bit
        // arithmetic) and their textures from an array indexed per hit.
        // The fragment shaders' quads share their rays.
        VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sub;
        vkGetPhysicalDeviceProperties2(m_gpu, &p2);
        const bool quads = (sub.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0 &&
                           (sub.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0;
        m_rayTracing = quads && v12.bufferDeviceAddress && asf.accelerationStructure && rqf.rayQuery &&
                       f2.features.shaderInt64 && v12.runtimeDescriptorArray &&
                       v12.shaderSampledImageArrayNonUniformIndexing && v12.descriptorBindingPartiallyBound;
    }
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (m_rayTracing) {
        exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.bufferDeviceAddress = VK_TRUE;
        v12.runtimeDescriptorArray = VK_TRUE;
        v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        v12.descriptorBindingPartiallyBound = VK_TRUE;
        enabled.shaderInt64 = VK_TRUE;
        asf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        asf.accelerationStructure = VK_TRUE;
        rqf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        rqf.rayQuery = VK_TRUE;
        v12.pNext = &asf;
        asf.pNext = &rqf;
        features.pNext = &v12;
        features.features = enabled;
    }

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
    ci.pQueueCreateInfos = queues.data();
    ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (m_rayTracing)
        ci.pNext = &features;
    else
        ci.pEnabledFeatures = &enabled;
    VK_CHECK_INIT(vkCreateDevice(m_gpu, &ci, nullptr, &m_device));
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    vkGetDeviceQueue(m_device, m_presentFamily, 0, &m_presentQueue);

    VmaVulkanFunctions fns{};
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = m_rayTracing ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    if (m_rayTracing)
        ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    ai.physicalDevice = m_gpu;
    ai.device = m_device;
    ai.instance = m_instance;
    VK_CHECK_INIT(vmaImportVulkanFunctionsFromVolk(&ai, &fns));
    ai.pVulkanFunctions = &fns;
    VK_CHECK_INIT(vmaCreateAllocator(&ai, &m_allocator));
    if (m_rayTracing) {
        m_rayScene.init(m_device, m_allocator, m_gpu, [this](std::function<void()> fn) { deferDelete(std::move(fn)); });
        log::info("vulkan: ray tracing available (VK_KHR_ray_query)");
    }
    return true;
}

VkFormat VulkanDevice::pickDepthFormat() const {
    const VkFormat formats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT,
                                VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM};
    // Preferably one the post passes can sample too (ambient occlusion).
    for (VkFormatFeatureFlags need :
         {VkFormatFeatureFlags(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
          VkFormatFeatureFlags(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)}) {
        for (VkFormat f : formats) {
            VkFormatProperties p;
            vkGetPhysicalDeviceFormatProperties(m_gpu, f, &p);
            if ((p.optimalTilingFeatures & need) == need)
                return f;
        }
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

    // OpenMM2 ray traced shadows: set 4, the frame's top level acceleration
    // structure (the mesh shaders' uScene).
    if (m_rayTracing) {
        // 0: the structure; 1: the alpha table (vk::RayAlpha); 2: the alpha
        // tested casters' textures (partly bound: slots fill as they come).
        VkDescriptorSetLayoutBinding rb[3]{};
        rb[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        rb[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        rb[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRayTextures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorBindingFlags rf[3] = {0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = 3;
        bf.pBindingFlags = rf;
        VkDescriptorSetLayoutCreateInfo rli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        rli.pNext = &bf;
        rli.bindingCount = 3;
        rli.pBindings = rb;
        VK_CHECK_INIT(vkCreateDescriptorSetLayout(m_device, &rli, nullptr, &m_raySetLayout));
        const VkDescriptorPoolSize sizes[3] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * kRayTextures}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = kFramesInFlight;
        dpi.poolSizeCount = 3;
        dpi.pPoolSizes = sizes;
        VK_CHECK_INIT(vkCreateDescriptorPool(m_device, &dpi, nullptr, &m_rayPool));
        for (auto& set : m_raySets) {
            VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            sa.descriptorPool = m_rayPool;
            sa.descriptorSetCount = 1;
            sa.pSetLayouts = &m_raySetLayout;
            VK_CHECK_INIT(vkAllocateDescriptorSets(m_device, &sa, &set));
        }
    }
    const VkDescriptorSetLayout sets[5] = {m_frameSetLayout, m_texSetLayout, m_texSetLayout, m_texSetLayout,
                                           m_raySetLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(detail::GpuTaaConstants)}; // the largest: taa.frag's
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = m_rayTracing ? 5 : 4;
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
    if (m_rayTracing)
        VK_CHECK_INIT(module(MM2_SHADER_SPV(mesh_ray_frag), m_meshFsRay));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(overlay_vert), m_overlayVs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(overlay_frag), m_overlayFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(composite_vert), m_compVs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(composite_frag), m_compFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(bloom_frag), m_bloomFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(depthcopy_frag), m_depthCopyFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(depthcopy_ms_frag), m_depthCopyMsFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(ao_frag), m_aoFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(aoblur_frag), m_aoBlurFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(ssr_frag), m_ssrFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(shafts_frag), m_shaftsFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(motionblur_frag), m_blurFs));
    VK_CHECK_INIT(module(MM2_SHADER_SPV(taa_frag), m_taaFs));
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
    {
        // OpenMM2 ambient occlusion samples the scene depth.
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(m_gpu, m_depthFormat, &fp);
        m_depthSampleable = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }
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
    m_postPass = createPostRenderPass(kSceneColorFormat);
    m_postPassBloom = createPostRenderPass(kBloomColorFormat);
    m_postPassR32 = createPostRenderPass(VK_FORMAT_R32_SFLOAT);
    m_postPassRG16 = createPostRenderPass(VK_FORMAT_R16G16_SFLOAT);
    createShadowRenderPass();
    createProbeRenderPass();
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
    m_rayScene.destroy();
    if (m_rayPool)
        vkDestroyDescriptorPool(m_device, m_rayPool, nullptr);
    if (m_raySetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_raySetLayout, nullptr);
    if (m_meshFsRay)
        vkDestroyShaderModule(m_device, m_meshFsRay, nullptr);
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
    destroyProbe(); // its colour image leaves m_textures
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
    destroyShadowMap(); // frees its set from the post pool
    if (m_postSetPool)
        vkDestroyDescriptorPool(m_device, m_postSetPool, nullptr);
    for (VkRenderPass rp :
         {m_postPass, m_postPassBloom, m_postPassR32, m_postPassRG16, m_shadowPass, m_shadowPassKeep, m_probePass,
          m_probePassKeep})
        if (rp)
            vkDestroyRenderPass(m_device, rp, nullptr);
    for (VkShaderModule m : {m_meshVs, m_meshFs, m_overlayVs, m_overlayFs, m_compVs, m_compFs, m_bloomFs,
                             m_depthCopyFs, m_depthCopyMsFs, m_aoFs, m_aoBlurFs, m_ssrFs, m_shaftsFs, m_blurFs,
                             m_taaFs})
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
    // Kept for the post passes (ambient occlusion) when they can sample it.
    depth.storeOp = m_depthSampleable ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = m_depthSampleable ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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
                   m_depthSampleable ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                     : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   m_samples,
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
    return createPostTargets();
}

VkRenderPass VulkanDevice::createPostRenderPass(VkFormat format) {
    // One colour attachment, its previous contents discarded, left readable
    // by the next pass's fragment shader.
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // In: the previous pass's (or frame's) writes become readable, and the
    // previous frame's reads of this image finish before it is overwritten.
    // Out: this image becomes readable by the next pass's fragment shader.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &pass), "vkCreateRenderPass(post)");
    return pass;
}

bool VulkanDevice::createPostTargets() {
    if (!m_postSetPool) {
        constexpr std::uint32_t kSets = 2 * detail::kBloomLevels + 8; // + the shadow map's
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = kSets;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_postSetPool) != VK_SUCCESS)
            return false;
    }
    auto allocSet = [&](VkImageView view, VkSampler smp, VkImageLayout layout, VkDescriptorSet& out) {
        VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        si.descriptorPool = m_postSetPool;
        si.descriptorSetCount = 1;
        si.pSetLayouts = &m_texSetLayout;
        if (vkAllocateDescriptorSets(m_device, &si, &out) != VK_SUCCESS)
            return false;
        VkDescriptorImageInfo img{smp, view, layout};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = out;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &img;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
        return true;
    };
    const VkSampler linear = sampler({Filter::Bilinear, AddressMode::Clamp, AddressMode::Clamp});
    auto make = [&](PostTarget& t, VkFormat format, VkRenderPass pass, std::uint32_t w, std::uint32_t h,
                    const std::string& name) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(m_allocator, &ii, &ai, &t.tex.image, &t.tex.alloc, nullptr) != VK_SUCCESS)
            return false;
        setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(t.tex.image), name);
        t.tex.width = w;
        t.tex.height = h;
        t.pass = pass;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.tex.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_device, &vi, nullptr, &t.tex.view) != VK_SUCCESS)
            return false;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &t.tex.view;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        if (vkCreateFramebuffer(m_device, &fi, nullptr, &t.framebuffer) != VK_SUCCESS)
            return false;
        return allocSet(t.tex.view, linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t.set);
    };
    std::uint32_t w = m_sceneExtent.width, h = m_sceneExtent.height;
    for (int i = 0; i < detail::kBloomLevels; ++i) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        if (!make(m_bloomDown[static_cast<std::size_t>(i)], kBloomColorFormat, m_postPassBloom, w, h,
                  std::format("bloom down {}", i)))
            return false;
        if (i + 1 < detail::kBloomLevels && !make(m_bloomUp[static_cast<std::size_t>(i)], kBloomColorFormat,
                                                  m_postPassBloom, w, h, std::format("bloom up {}", i)))
            return false;
    }
    m_bloomThisFrame = false;
    m_aoThisFrame = false;
    if (m_depthSampleable) {
        const std::uint32_t sw = m_sceneExtent.width, sh = m_sceneExtent.height;
        if (!make(m_aoDepth, VK_FORMAT_R32_SFLOAT, m_postPassR32, sw, sh, "ao distance") ||
            !make(m_aoRaw, VK_FORMAT_R16G16_SFLOAT, m_postPassRG16, sw, sh, "ao occlusion") ||
            !make(m_aoScene, kSceneColorFormat, m_postPass, sw, sh, "ao scene") ||
            !make(m_ssrScene, kSceneColorFormat, m_postPass, sw, sh, "wet scene") ||
            !make(m_shaftScene, kSceneColorFormat, m_postPass, sw, sh, "shafts scene") ||
            !make(m_blurScene, kSceneColorFormat, m_postPass, sw, sh, "blurred scene") ||
            !make(m_taa[0], kSceneColorFormat, m_postPass, m_output.width, m_output.height, "taa 0") ||
            !make(m_taa[1], kSceneColorFormat, m_postPass, m_output.width, m_output.height, "taa 1"))
            return false;
        m_taaValid = false;
        const VkSampler point = sampler({Filter::Point, AddressMode::Clamp, AddressMode::Clamp});
        if (!allocSet(m_sceneDepth.view, point, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, m_sceneDepthSet))
            return false;
    }
    return true;
}

void VulkanDevice::destroyPostTargets() {
    auto destroy = [&](PostTarget& t) {
        if (t.set)
            vkFreeDescriptorSets(m_device, m_postSetPool, 1, &t.set);
        if (t.framebuffer)
            vkDestroyFramebuffer(m_device, t.framebuffer, nullptr);
        if (t.tex.view)
            vkDestroyImageView(m_device, t.tex.view, nullptr);
        if (t.tex.image)
            vmaDestroyImage(m_allocator, t.tex.image, t.tex.alloc);
        t = {};
    };
    for (auto& t : m_bloomDown)
        destroy(t);
    for (auto& t : m_bloomUp)
        destroy(t);
    for (PostTarget* t : {&m_aoDepth, &m_aoRaw, &m_aoScene, &m_ssrScene, &m_shaftScene, &m_blurScene, &m_taa[0], &m_taa[1]})
        destroy(*t);
    m_taaValid = false;
    m_ssrThisFrame = false;
    m_postCurrent = nullptr;
    if (m_sceneDepthSet) {
        vkFreeDescriptorSets(m_device, m_postSetPool, 1, &m_sceneDepthSet);
        m_sceneDepthSet = VK_NULL_HANDLE;
    }
    m_bloomThisFrame = false;
    m_aoThisFrame = false;
}

void VulkanDevice::drawPostPass(VkCommandBuffer cmd, const PostTarget& dst, VkPipeline pipeline, VkDescriptorSet set1,
                                VkDescriptorSet set2, const void* constants, std::uint32_t size,
                                VkDescriptorSet set3) {
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = dst.pass;
    rb.framebuffer = dst.framebuffer;
    rb.renderArea = {{0, 0}, {dst.tex.width, dst.tex.height}};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    // Engine y-up viewport, as every other pass (setViewport).
    const VkViewport vp{0, static_cast<float>(dst.tex.height), static_cast<float>(dst.tex.width),
                        -static_cast<float>(dst.tex.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, {dst.tex.width, dst.tex.height}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    if (set1)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &set1, 0, nullptr);
    if (set2)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 2, 1, &set2, 0, nullptr);
    if (set3)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 3, 1, &set3, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size,
                       constants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

VkDescriptorSet VulkanDevice::postSourceSet() const { return m_postCurrent ? m_postCurrent->set : m_sceneSet; }
const VkTex& VulkanDevice::postSourceTex() const { return m_postCurrent ? m_postCurrent->tex : m_sceneColor; }

void VulkanDevice::runAo(VkCommandBuffer cmd) {
    const PostProcess& post = postProcess();
    m_aoThisFrame = false;
    m_ssrThisFrame = false;
    m_postCurrent = nullptr;
    // The distance image feeds the ambient occlusion, the wet roads and the
    // sun shafts.
    const bool ao = post.ao > 0.0f;
    // (The heat shimmer is drawn in the wet roads' pass.)
    const bool wet = (post.wetness > 0.0f || post.heat > 0.0f) && sceneCamera().has_value();
    const bool sun = post.shafts > 0.0f && sceneCamera().has_value() && sceneSun().has_value();
    const bool moving = post.motionBlur > 0.0f && sceneCamera().has_value() && m_prevCamera && m_prevProjection;
    const bool taa = post.taa && sceneCamera().has_value();
    if (!taa)
        m_taaValid = false; // its history is stale when it comes back
    // The camera for the next frame's motion blur and anti-aliasing (or none).
    struct KeepPrevious {
        VulkanDevice& d;
        ~KeepPrevious() {
            d.m_prevProjection = d.sceneProjection();
            d.m_prevCamera = d.sceneCamera();
            d.m_prevTime = d.postProcess().time;
        }
    } keep{*this};
    if (!m_sceneRendered || !(ao || wet || sun || moving || taa) || !sceneProjection() || !m_sceneDepthSet) {
        m_taaValid = false;
        return;
    }
    const bool msaa = m_samples != VK_SAMPLE_COUNT_1_BIT;
    if (!m_depthCopyPipeline)
        m_depthCopyPipeline = fullscreenPipeline(msaa ? m_depthCopyMsFs : m_depthCopyFs, m_postPassR32, "depth copy");
    if (!m_aoPipeline)
        m_aoPipeline = fullscreenPipeline(m_aoFs, m_postPassRG16, "ao");
    if (!m_aoBlurPipeline)
        m_aoBlurPipeline = fullscreenPipeline(m_aoBlurFs, m_postPass, "ao blur");
    if (!m_ssrPipeline)
        m_ssrPipeline = fullscreenPipeline(m_ssrFs, m_postPass, "wet roads");
    if (!m_shaftsPipeline)
        m_shaftsPipeline = fullscreenPipeline(m_shaftsFs, m_postPass, "sun shafts");
    if (!m_blurPipeline)
        m_blurPipeline = fullscreenPipeline(m_blurFs, m_postPass, "motion blur");
    if (!m_taaPipeline)
        m_taaPipeline = fullscreenPipeline(m_taaFs, m_postPass, "taa");
    if (!m_depthCopyPipeline || !m_aoPipeline || !m_aoBlurPipeline || !m_ssrPipeline || !m_shaftsPipeline ||
        !m_blurPipeline)
        return;
    const Mat44& proj = *sceneProjection();
    const auto copy = detail::depthCopyConstants(proj);
    drawPostPass(cmd, m_aoDepth, m_depthCopyPipeline, m_sceneDepthSet, VK_NULL_HANDLE, &copy, sizeof(copy));
    if (ao) {
        const auto aoc = detail::aoConstants(proj, post, m_aoDepth.tex.width, m_aoDepth.tex.height);
        drawPostPass(cmd, m_aoRaw, m_aoPipeline, m_aoDepth.set, VK_NULL_HANDLE, &aoc, sizeof(aoc));
        const auto blur = detail::aoBlurConstants(post, m_aoRaw.tex.width, m_aoRaw.tex.height);
        drawPostPass(cmd, m_aoScene, m_aoBlurPipeline, m_aoRaw.set, m_sceneSet, &blur, sizeof(blur));
        m_aoThisFrame = true;
        m_postCurrent = &m_aoScene;
    }
    if (wet) {
        const auto sc = detail::ssrConstants(proj, *sceneCamera(), post, sceneSky());
        drawPostPass(cmd, m_ssrScene, m_ssrPipeline, postSourceSet(), m_aoDepth.set, &sc, sizeof(sc));
        m_ssrThisFrame = true;
        m_postCurrent = &m_ssrScene;
    }
    if (sun) {
        const auto sc = detail::shaftsConstants(proj, *sceneCamera(), sceneSun()->direction, sceneSun()->color, post);
        if (sc.a[2] > 0.0f && sc.a[3] > 0.0f) {
            drawPostPass(cmd, m_shaftScene, m_shaftsPipeline, postSourceSet(), m_aoDepth.set, &sc, sizeof(sc));
            m_postCurrent = &m_shaftScene;
        }
    }
    if (moving) {
        const auto mc = detail::motionBlurConstants(proj, *sceneCamera(), *m_prevProjection, *m_prevCamera, post,
                                                    post.time - m_prevTime, sceneFocus());
        if (mc.params[3] > 0.0f) {
            drawPostPass(cmd, m_blurScene, m_blurPipeline, postSourceSet(), m_aoDepth.set, &mc, sizeof(mc));
            m_postCurrent = &m_blurScene;
        }
    }
    if (taa && m_taaPipeline && m_taa[0].framebuffer) {
        PostTarget& out = m_taa[m_taaIndex];
        const PostTarget& history = m_taa[m_taaIndex ^ 1u];
        const bool reset = !m_taaValid || !m_prevCamera || !m_prevProjection;
        const auto tc = detail::taaConstants(proj, *sceneCamera(), m_prevProjection ? *m_prevProjection : proj,
                                             m_prevCamera ? *m_prevCamera : *sceneCamera(), taaJitter(), reset,
                                             false, sceneFocus(), stillRect(), m_sceneExtent);
        // Without a history yet (never written: its layout undefined) the
        // frame itself stands in; it is not read (weight 0).
        drawPostPass(cmd, out, m_taaPipeline, postSourceSet(), m_aoDepth.set, &tc, sizeof(tc),
                     reset ? postSourceSet() : history.set);
        m_postCurrent = &out;
        m_taaIndex ^= 1u;
        m_taaValid = true;
    }
}

void VulkanDevice::runBloom(VkCommandBuffer cmd) {
    const PostProcess& post = postProcess();
    m_bloomThisFrame = false;
    if (!m_sceneRendered || post.bloom <= 0.0f || !m_bloomUp[0].framebuffer)
        return;
    if (!m_bloomPipeline)
        m_bloomPipeline = fullscreenPipeline(m_bloomFs, m_postPassBloom, "bloom");
    if (!m_bloomPipeline)
        return;
    auto pass = [&](const PostTarget& dst, VkDescriptorSet src, const VkTex& srcTex, VkDescriptorSet add,
                    const VkTex* addTex, float mode) {
        detail::GpuBloomConstants bc{};
        bc.params[0] = mode;
        bc.params[1] = post.bloomThreshold;
        bc.texel[0] = 1.0f / static_cast<float>(srcTex.width);
        bc.texel[1] = 1.0f / static_cast<float>(srcTex.height);
        if (addTex) {
            bc.texel[2] = 1.0f / static_cast<float>(addTex->width);
            bc.texel[3] = 1.0f / static_cast<float>(addTex->height);
        }
        drawPostPass(cmd, dst, m_bloomPipeline, src, add, &bc, sizeof(bc));
    };
    // Down: the bright pass from the scene, then halving. The second
    // texture is unread there but must be valid: the source again.
    pass(m_bloomDown[0], postSourceSet(), postSourceTex(), postSourceSet(), nullptr, 0.0f);
    for (std::size_t i = 1; i < m_bloomDown.size(); ++i)
        pass(m_bloomDown[i], m_bloomDown[i - 1].set, m_bloomDown[i - 1].tex, m_bloomDown[i - 1].set, nullptr, 1.0f);
    // Up: each level's down image mixed with the blurred next smaller level.
    const std::size_t last = m_bloomUp.size() - 1;
    pass(m_bloomUp[last], m_bloomDown[last].set, m_bloomDown[last].tex, m_bloomDown[last + 1].set,
         &m_bloomDown[last + 1].tex, 2.0f);
    for (std::size_t i = last; i-- > 0;)
        pass(m_bloomUp[i], m_bloomDown[i].set, m_bloomDown[i].tex, m_bloomUp[i + 1].set, &m_bloomUp[i + 1].tex, 2.0f);
    m_bloomThisFrame = true;
}

VkPipeline VulkanDevice::fullscreenPipeline(VkShaderModule fs, VkRenderPass pass, const char* name) {
    // composite.vert's full-screen triangle with `fs`, no blending, depth or
    // culling; viewport and scissor dynamic.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_compVs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
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
    ci.renderPass = pass;
    VkPipeline p = VK_NULL_HANDLE;
    logVk(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &p),
          std::format("vkCreateGraphicsPipelines({})", name).c_str());
    return p;
}

void VulkanDevice::destroySceneTargets() {
    destroyPostTargets();
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
        destroyShadowMap(); // its descriptor holds a sampler; the next shadow pass makes a new one
        m_boundShadowSet = VK_NULL_HANDLE;
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
    if (!sceneOnly) {
        for (VkPipeline* pl : {&m_bloomPipeline, &m_aoPipeline, &m_aoBlurPipeline, &m_ssrPipeline, &m_shaftsPipeline,
                               &m_blurPipeline, &m_taaPipeline})
            if (*pl) {
                vkDestroyPipeline(m_device, *pl, nullptr);
                *pl = VK_NULL_HANDLE;
            }
    }
    // The depth copy's shader depends on the scene's sample count.
    if (m_depthCopyPipeline) {
        vkDestroyPipeline(m_device, m_depthCopyPipeline, nullptr);
        m_depthCopyPipeline = VK_NULL_HANDLE;
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
    // With ray tracing the scene's and the probe's mesh draws use the
    // shader that can trace their sun shadows (it reads the shadow map when
    // the frame does not ask for rays).
    const bool rays = m_rayTracing && (pass == Pass::Scene || pass == Pass::Probe);
    stages[1].module = mesh ? (rays ? m_meshFsRay : m_meshFs) : m_overlayFs;
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
    const bool hasDepth = pass == Pass::Scene || pass == Pass::Shadow || pass == Pass::Probe;
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
    case BlendMode::Shaded:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = pass == Pass::Shadow ? 0 : 1; // the shadow pass has depth only
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
    ci.renderPass = pass == Pass::Scene    ? m_scenePass
                    : pass == Pass::Shadow ? m_shadowPass
                    : pass == Pass::Probe  ? m_probePass
                                           : m_outputPass;
    ci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    const auto start = std::chrono::steady_clock::now();
    logVk(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
    if (trace::active()) // OpenMM2 hitch log: a pipeline made on first use
        trace::note(std::format("pipeline {:.1f} ms", trace::msSince(start)));
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
            const bool geometry = (ring.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0;
            if (m_rayTracing && geometry)
                bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
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
            b.transient = true;
            if (m_rayTracing && geometry)
                b.address = bufferAddress(b.buffer);
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
    if (m_rayTracing && !m_rayTextures.empty()) {
        for (std::uint32_t slot = 1; slot < m_rayTextures.size(); ++slot)
            if (m_rayTextures[slot].id == handle.id) {
                m_rayTextures[slot] = m_rayTextures[0]; // white until the set is written again
                for (auto& d : m_rayTexturesDirty)
                    d.push_back(slot);
                m_rayFreedSlots.push_back({slot, m_rayFrames + 1000});
            }
        std::erase_if(m_rayTextureSlots, [&](const auto& e) { return (e.first >> 32) == handle.id; });
    }
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
    if (m_rayTracing)
        bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (VkResult r = vmaCreateBuffer(m_allocator, &bi, &ai, &b.buffer, &b.alloc, nullptr); r != VK_SUCCESS) {
        logVk(r, "vmaCreateBuffer");
        return {};
    }
    if (m_rayTracing)
        b.address = bufferAddress(b.buffer);
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
    if (m_rayTracing)
        m_rayScene.forgetBuffer(handle.id);
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

    ++m_rayFrames; // the ray texture slots' clock (rayTextureSlot)
    Frame& f = m_frames[m_frameIndex];
    // OpenMM2 hitch log: the waits and the deletions, when they are slow.
    auto slow = [](std::string_view what, std::chrono::steady_clock::time_point since) {
        if (trace::active())
            if (const double ms = trace::msSince(since); ms >= 2.0)
                trace::note(std::format("{} {:.1f} ms", what, ms));
    };
    auto t0 = std::chrono::steady_clock::now();
    vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    slow("gpuwait", t0);
    t0 = std::chrono::steady_clock::now();
    const std::size_t deletions = f.deletions.size();
    for (auto& d : f.deletions)
        d();
    f.deletions.clear();
    slow(std::format("deletions({})", deletions), t0);
    t0 = std::chrono::steady_clock::now();

    VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);
    slow("acquire", t0);
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
    m_shadowThisFrame = false;
    m_probeThisFrame = false;
    m_rayTop = VK_NULL_HANDLE;
    m_boundShadowSet = VK_NULL_HANDLE;
    m_pass = Pass::None;
    return true;
}

Extent2D VulkanDevice::passExtent() const {
    if (m_pass == Pass::Scene)
        return m_sceneExtent;
    if (m_pass == Pass::Shadow)
        return {m_shadowSize, m_shadowSize};
    if (m_pass == Pass::Probe)
        return m_probeExtent;
    return m_output;
}

void VulkanDevice::bindDefaultViewport() {
    const Extent2D e = passExtent();
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
    if (m_rayTracing)
        m_rayScene.buildEmpty(f.cmd); // the set needs a structure to name
    vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    bindRaySet(f.cmd);
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

// --- OpenMM2 sun shadows ----------------------------------------------------------

void VulkanDevice::createShadowRenderPass() {
    // Depth only, cleared, kept for the scene pass's fragment shaders.
    VkAttachmentDescription depth{};
    depth.format = m_depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &ref;
    // In: the previous frame's reads finish before the map is rewritten,
    // and its depth writes are visible to the keeping pass's load (both
    // passes declare the same). Out: the depth writes become readable by
    // the scene's fragment shaders.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &depth;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &m_shadowPass), "vkCreateRenderPass(shadow)");
    // The same, keeping what the last frames drew (compatible with the
    // clearing pass, so the pipelines and framebuffer serve both).
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &m_shadowPassKeep), "vkCreateRenderPass(shadow, keep)");
}

bool VulkanDevice::ensureShadowMap(std::uint32_t size) {
    if (m_shadowSize == size && m_shadowFramebuffer)
        return true;
    if (m_shadowFramebuffer) {
        vkDeviceWaitIdle(m_device); // the previous frame may still read the old map
        destroyShadowMap();
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = m_depthFormat;
    ii.extent = {size, size, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(m_allocator, &ii, &ai, &m_shadowTex.image, &m_shadowTex.alloc, nullptr) != VK_SUCCESS)
        return false;
    setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(m_shadowTex.image), "shadow map");
    m_shadowTex.width = m_shadowTex.height = size;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_shadowTex.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = m_depthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &vi, nullptr, &m_shadowTex.view) != VK_SUCCESS)
        return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = m_shadowPass;
    fi.attachmentCount = 1;
    fi.pAttachments = &m_shadowTex.view;
    fi.width = size;
    fi.height = size;
    fi.layers = 1;
    if (vkCreateFramebuffer(m_device, &fi, nullptr, &m_shadowFramebuffer) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    si.descriptorPool = m_postSetPool;
    si.descriptorSetCount = 1;
    si.pSetLayouts = &m_texSetLayout;
    if (!m_postSetPool || vkAllocateDescriptorSets(m_device, &si, &m_shadowSet) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo img{sampler({Filter::Point, AddressMode::Clamp, AddressMode::Clamp}), m_shadowTex.view,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = m_shadowSet;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
    m_shadowSize = size;
    log::debug("vulkan: shadow map {}x{}", size, size);
    return true;
}

void VulkanDevice::destroyShadowMap() {
    if (m_shadowSet)
        vkFreeDescriptorSets(m_device, m_postSetPool, 1, &m_shadowSet);
    m_shadowSet = VK_NULL_HANDLE;
    if (m_shadowFramebuffer)
        vkDestroyFramebuffer(m_device, m_shadowFramebuffer, nullptr);
    m_shadowFramebuffer = VK_NULL_HANDLE;
    if (m_shadowTex.view)
        vkDestroyImageView(m_device, m_shadowTex.view, nullptr);
    if (m_shadowTex.image)
        vmaDestroyImage(m_allocator, m_shadowTex.image, m_shadowTex.alloc);
    m_shadowTex = {};
    m_shadowSize = 0;
    m_shadowDrawn = false;
    m_shadowThisFrame = false;
    m_boundShadowSet = VK_NULL_HANDLE;
}

bool VulkanDevice::beginShadowPass(std::uint32_t size, bool keep) {
    if (!m_inFrame || m_pass != Pass::None || !m_depthSampleable || !m_shadowPass || size == 0)
        return false;
    if (!ensureShadowMap(size))
        return false;
    m_shadowKept = keep && m_shadowDrawn && m_shadowPassKeep;
    m_shadowDrawn = true;
    Frame& f = m_frames[m_frameIndex];
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = m_shadowKept ? m_shadowPassKeep : m_shadowPass;
    rb.framebuffer = m_shadowFramebuffer;
    rb.renderArea = {{0, 0}, {size, size}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    m_pass = Pass::Shadow;
    m_shadowThisFrame = false; // the map is being written: draws bind white
    m_boundPipeline = VK_NULL_HANDLE;
    m_boundVb = m_boundIb = VK_NULL_HANDLE;
    m_boundTexSet[0] = m_boundTexSet[1] = VK_NULL_HANDLE;
    m_boundShadowSet = VK_NULL_HANDLE;
    m_frameSetBound = false;
    bindDefaultViewport();
    return true;
}

void VulkanDevice::endShadowPass() {
    if (m_pass != Pass::Shadow)
        return;
    vkCmdEndRenderPass(m_frames[m_frameIndex].cmd);
    m_pass = Pass::None;
    m_shadowThisFrame = true;
    m_boundShadowSet = VK_NULL_HANDLE;
}

void VulkanDevice::bindShadowSet(VkCommandBuffer cmd) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    if ((m_pass == Pass::Scene || m_pass == Pass::Probe) && m_shadowThisFrame && m_shadowSet) {
        set = m_shadowSet;
    } else if (const VkTex* white = m_textures.get(m_white.id)) {
        set = textureSet(m_white.id, white->view, SamplerDesc{});
    }
    if (set && set != m_boundShadowSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 3, 1, &set, 0, nullptr);
        m_boundShadowSet = set;
    }
}

// --- OpenMM2 live reflections -----------------------------------------------------

void VulkanDevice::createProbeRenderPass() {
    // Colour kept for later fragment shaders this frame; depth only while
    // the pass draws.
    VkAttachmentDescription atts[2]{};
    atts[0].format = kSceneColorFormat;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    atts[1].format = m_depthFormat;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color;
    sub.pDepthStencilAttachment = &depth;
    // In: the previous frame's reads and depth writes finish before the
    // images are rewritten, and its colour writes are visible to the
    // keeping pass's loads (both passes declare the same, as passes sharing
    // a framebuffer must). Out: the colour becomes readable by the scene's
    // fragment shaders.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 2;
    ci.pAttachments = atts;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &m_probePass), "vkCreateRenderPass(probe)");
    // The same, keeping the colour the last frames drew (compatible with the
    // clearing pass, so the pipelines and framebuffer serve both).
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    logVk(vkCreateRenderPass(m_device, &ci, nullptr, &m_probePassKeep), "vkCreateRenderPass(probe, keep)");
}

bool VulkanDevice::ensureProbe(std::uint32_t width, std::uint32_t height) {
    if (m_probeExtent.width == width && m_probeExtent.height == height && m_probeFramebuffer)
        return true;
    if (m_probeFramebuffer) {
        vkDeviceWaitIdle(m_device); // the previous frame may still read the old image
        destroyProbe();
    }
    auto image = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkTex& out,
                     const char* name) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {width, height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(m_allocator, &ii, &ai, &out.image, &out.alloc, nullptr) != VK_SUCCESS)
            return false;
        setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(out.image), name);
        out.width = width;
        out.height = height;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        return vkCreateImageView(m_device, &vi, nullptr, &out.view) == VK_SUCCESS;
    };
    VkTex color;
    if (!image(kSceneColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, color, "reflection probe")) {
        if (color.view)
            vkDestroyImageView(m_device, color.view, nullptr);
        if (color.image)
            vmaDestroyImage(m_allocator, color.image, color.alloc);
        return false;
    }
    m_probeId = m_textures.insert(color);
    if (!image(m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, m_probeDepth,
               "reflection probe depth")) {
        destroyProbe();
        return false;
    }
    const VkImageView views[2] = {color.view, m_probeDepth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = m_probePass;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = width;
    fi.height = height;
    fi.layers = 1;
    if (vkCreateFramebuffer(m_device, &fi, nullptr, &m_probeFramebuffer) != VK_SUCCESS) {
        destroyProbe();
        return false;
    }
    m_probeExtent = {width, height};
    log::debug("vulkan: reflection probe {}x{}", width, height);
    return true;
}

void VulkanDevice::destroyProbe() {
    if (m_probeFramebuffer)
        vkDestroyFramebuffer(m_device, m_probeFramebuffer, nullptr);
    m_probeFramebuffer = VK_NULL_HANDLE;
    if (m_probeId) {
        forgetTextureSets(m_probeId);
        if (auto t = m_textures.remove(m_probeId)) {
            if (t->view)
                vkDestroyImageView(m_device, t->view, nullptr);
            if (t->image)
                vmaDestroyImage(m_allocator, t->image, t->alloc);
        }
    }
    m_probeId = 0;
    if (m_probeDepth.view)
        vkDestroyImageView(m_device, m_probeDepth.view, nullptr);
    if (m_probeDepth.image)
        vmaDestroyImage(m_allocator, m_probeDepth.image, m_probeDepth.alloc);
    m_probeDepth = {};
    m_probeExtent = {};
    m_probeThisFrame = false;
    m_probeDrawn = false;
}

bool VulkanDevice::beginProbePass(std::uint32_t width, std::uint32_t height, bool keep) {
    if (!m_inFrame || m_pass != Pass::None || !m_probePass || width == 0 || height == 0)
        return false;
    if (!ensureProbe(width, height))
        return false;
    m_probeKept = keep && m_probeDrawn && m_probePassKeep;
    m_probeDrawn = true;
    Frame& f = m_frames[m_frameIndex];
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = m_probeKept ? m_probePassKeep : m_probePass;
    rb.framebuffer = m_probeFramebuffer;
    rb.renderArea = {{0, 0}, {width, height}};
    rb.clearValueCount = 2;
    rb.pClearValues = clears;
    if (m_rayTracing)
        m_rayScene.buildEmpty(f.cmd);
    vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    bindRaySet(f.cmd);
    m_pass = Pass::Probe;
    m_probeThisFrame = false; // being written: probeTexture() is null
    m_boundPipeline = VK_NULL_HANDLE;
    m_boundVb = m_boundIb = VK_NULL_HANDLE;
    m_boundTexSet[0] = m_boundTexSet[1] = VK_NULL_HANDLE;
    m_boundShadowSet = VK_NULL_HANDLE;
    m_frameSetBound = false;
    bindDefaultViewport();
    return true;
}

void VulkanDevice::endProbePass() {
    if (m_pass != Pass::Probe)
        return;
    vkCmdEndRenderPass(m_frames[m_frameIndex].cmd);
    m_pass = Pass::None;
    m_probeThisFrame = true;
    m_boundShadowSet = VK_NULL_HANDLE;
}

void VulkanDevice::beginOverlay(const Vec4& clearColor) {
    Frame& f = m_frames[m_frameIndex];
    runAo(f.cmd); // OpenMM2 post-processing, before the output pass
    runBloom(f.cmd);
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
        // Set 2: the bloom (composite.frag reads it only with bloom on; the
        // scene again otherwise, so the binding is always valid).
        const VkDescriptorSet sets[2] = {postSourceSet(), m_bloomThisFrame ? m_bloomUp[0].set : m_sceneSet};
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 2, sets, 0, nullptr);
        // OpenMM2 image filters (composite.frag's push constants).
        detail::GpuPostConstants pc = detail::toGpu(postProcess(), m_settings.renderScale);
        if (!m_bloomThisFrame)
            pc.detail[3] = 0.0f;
        detail::setUnfilteredRects(pc, unfilteredRects(), m_sceneExtent, false);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 3, 1, &m_sceneSet, 0, nullptr);
        m_boundShadowSet = VK_NULL_HANDLE; // the next mesh draw binds its own set 3
        vkCmdPushConstants(f.cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
        vkCmdDraw(f.cmd, 3, 1, 0, 0);
        m_boundPipeline = m_compositePipeline;
        m_boundTexSet[0] = sets[0];
        m_boundTexSet[1] = sets[1];
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
    const Extent2D e = passExtent();
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
    const Extent2D e = passExtent();
    VkClearAttachment atts[2]{};
    std::uint32_t n = 0;
    if (cv.clearColor) {
        atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        atts[n].colorAttachment = 0;
        atts[n].clearValue.color = {{cv.color.x, cv.color.y, cv.color.z, cv.color.w}};
        ++n;
    }
    if (cv.clearDepth && (m_pass == Pass::Scene || m_pass == Pass::Probe || m_pass == Pass::Shadow)) {
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

// --- OpenMM2 ray traced sun shadows --------------------------------------------------

VkDeviceAddress VulkanDevice::bufferAddress(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = buffer;
    return vkGetBufferDeviceAddress(m_device, &i);
}

bool VulkanDevice::beginRayScene(bool statics, bool first, bool last) {
    if (!m_rayTracing || !m_inFrame || m_pass != Pass::None)
        return false;
    // A kept layer that cannot be used is captured whole now.
    if (m_rayScene.staticsStale())
        m_rayScene.beginCapture(true, m_frameIndex);
    else
        m_rayScene.beginCapture(statics, m_frameIndex, first, last);
    m_pass = Pass::RayScene;
    return true;
}

void VulkanDevice::captureRay(const DrawCall& drawn) {
    // What casts in the shadow pass is seen by every ray (alpha tested draws,
    // trees and fences, as geometry the rays test against their texture's
    // alpha); the sky dome (no shadow, no depth) and the lights' glows
    // (added) only by the reflections'.
    std::optional<DrawCall> caster = shadowCasterCall(drawn);
    const bool sky = !caster && (drawn.constants.flags & DrawFlag::NoShadow) != 0 && !drawn.state.depthWrite &&
                     drawn.state.blend == BlendMode::Opaque;
    // (Not a reflection map added over a surface, as on the cars' paint: a
    // ray saw it as a light, a flat bright shape of the car.)
    constexpr std::uint32_t kReflectionMap = DrawFlag::Texture1 | DrawFlag::EnvMap1 | DrawFlag::EnvProbe;
    const bool glow = !caster && !sky && (drawn.constants.flags & kReflectionMap) == 0 &&
                      (drawn.state.blend == BlendMode::Add || drawn.state.blend == BlendMode::Additive);
    if (!caster && !sky && !glow)
        return;
    const DrawCall& c = caster ? *caster : drawn;
    if (c.state.vertexFormat != VertexFormat::Mesh || c.state.topology != Topology::TriangleList || c.count < 3)
        return;
    const VkBuf* vb = m_buffers.get(c.vertices.buffer.id);
    if (!vb || !vb->address)
        return;
    const VkBuf* ib = c.indices ? m_buffers.get(c.indices.buffer.id) : nullptr;
    if (c.indices && (!ib || !ib->address))
        return;
    vk::RayGeometry g;
    constexpr std::uint32_t stride = sizeof(Vertex3D);
    g.vertices = vb->address + c.vertices.offset;
    g.stride = stride;
    const VkDeviceSize vertexBytes = vb->size > c.vertices.offset ? vb->size - c.vertices.offset : 0;
    if (vertexBytes < stride)
        return;
    g.maxVertex = static_cast<std::uint32_t>(vertexBytes / stride - 1);
    g.triangles = c.count / 3;
    if (ib) {
        if (c.baseVertex < 0)
            return;
        g.indices = ib->address + c.indices.offset;
        g.indexType = c.indexType == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        g.indexOffset = c.first * (c.indexType == IndexType::U16 ? 2u : 4u);
        g.firstVertex = static_cast<std::uint32_t>(c.baseVertex);
    } else {
        g.indexOffset = c.first * stride;
    }
    // Its material: what the shaders read where a ray meets it.
    vk::RayMaterial m;
    m.vertices = g.vertices;
    m.indices = g.indices;
    m.indexBytes = ib ? (c.indexType == IndexType::U16 ? 2u : 4u) : 0u;
    m.firstVertex = g.firstVertex;
    m.firstIndex = c.first;
    m.alphaRef = c.constants.alphaRef;
    m.color[0] = c.constants.color.x;
    m.color[1] = c.constants.color.y;
    m.color[2] = c.constants.color.z;
    m.color[3] = c.constants.color.w;
    const std::uint32_t f = c.constants.flags;
    if ((f & DrawFlag::VertexColor) != 0)
        m.flags |= vk::RayMaterial::kVertexColor;
    if ((f & DrawFlag::Lighting) != 0)
        m.flags |= vk::RayMaterial::kLighting;
    if ((f & DrawFlag::NightLit) != 0)
        m.flags |= vk::RayMaterial::kNightLit;
    if (sky)
        m.flags |= vk::RayMaterial::kSky;
    if (glow) {
        m.flags |= vk::RayMaterial::kGlow;
        g.opaque = false;
    }
    if ((f & DrawFlag::Texture0) != 0)
        m.texture = rayTextureSlot(c.textures[0]);
    if ((f & DrawFlag::AlphaTest) != 0) {
        if (m.texture == 0)
            return; // no texture, or no slot left
        g.opaque = false;
        m.flags |= vk::RayMaterial::kAlphaTest;
    }
    // The static layer outlives per-frame memory: its indices are kept (and
    // per-frame vertices it has none of; such a draw is left out).
    if (m_rayScene.layer() == vk::RayScene::Layer::Static && m_rayScene.capturingStatics()) {
        if (vb->transient)
            return;
        if (ib && ib->transient) {
            const auto* src = static_cast<const std::uint8_t*>(ib->mapped) + c.indices.offset + c.first * m.indexBytes;
            m.indices = m_rayScene.keepStatic(src, static_cast<std::size_t>(c.count) * m.indexBytes);
            if (!m.indices)
                return;
            m.firstIndex = 0;
        }
    }
    const bool persistent = !vb->transient && (!ib || !ib->transient);
    vk::RayKey key{c.vertices.buffer.id, c.vertices.offset, ib ? c.indices.buffer.id : 0u,
                   ib ? c.indices.offset : 0u, c.first, c.count, c.baseVertex, g.opaque};
    // Its faces as the game draws it (`drawn`: the shadow pass's caster has
    // culling off): a mirroring transform turns the winding the raster sees
    // but not the rays'.
    const auto& w = c.constants.world.m;
    const float det = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1]) -
                      w[0][1] * (w[1][0] * w[2][2] - w[1][2] * w[2][0]) +
                      w[0][2] * (w[1][0] * w[2][1] - w[1][1] * w[2][0]);
    const bool twoSided = drawn.state.cull == CullMode::None;
    const bool flip = (drawn.state.frontFace == FrontFace::Clockwise) != (det < 0.0f) != kRayFlipFacing;
    m_rayScene.add(g, persistent ? &key : nullptr, c.constants.world.m, m,
                   sky    ? vk::kRaySky
                   : glow ? vk::kRayReflect
                          : static_cast<std::uint8_t>(vk::kRayShadow | vk::kRayReflect),
                   twoSided, flip);
}

std::uint32_t VulkanDevice::rayTextureSlot(const TextureBinding& binding) {
    const VkTex* t = m_textures.get(binding.texture.id);
    if (!t)
        return 0;
    const std::uint64_t key = (static_cast<std::uint64_t>(binding.texture.id) << 32) | binding.sampler.key();
    if (auto it = m_rayTextureSlots.find(key); it != m_rayTextureSlots.end())
        return it->second;
    if (m_rayTextures.empty()) {
        const VkTex* white = m_textures.get(m_white.id);
        m_rayTextures.push_back({m_white.id, white ? white->view : VK_NULL_HANDLE, sampler(SamplerDesc{})});
    }
    while (!m_rayFreedSlots.empty() && m_rayFreedSlots.front().freeAt <= m_rayFrames) {
        m_rayFreeSlots.push_back(m_rayFreedSlots.front().slot);
        m_rayFreedSlots.pop_front();
    }
    std::uint32_t slot = 0;
    if (!m_rayFreeSlots.empty()) {
        slot = m_rayFreeSlots.back();
        m_rayFreeSlots.pop_back();
        m_rayTextures[slot] = {binding.texture.id, t->view, sampler(binding.sampler)};
        for (auto& d : m_rayTexturesDirty)
            d.push_back(slot);
    } else {
        if (m_rayTextures.size() >= kRayTextures && !m_rayFreedSlots.empty()) {
            // Full: the longest freed slot at once (a stale reflection's
            // texture for a moment rather than white from now on).
            slot = m_rayFreedSlots.front().slot;
            m_rayFreedSlots.pop_front();
            m_rayTextures[slot] = {binding.texture.id, t->view, sampler(binding.sampler)};
            for (auto& d : m_rayTexturesDirty)
                d.push_back(slot);
            m_rayTextureSlots.emplace(key, slot);
            return slot;
        }
        if (m_rayTextures.size() >= kRayTextures) {
            static bool warned = false;
            if (!std::exchange(warned, true))
                log::warn("vulkan: the ray scene's {} texture slots are full; later textures read as white",
                          kRayTextures);
            return 0;
        }
        slot = static_cast<std::uint32_t>(m_rayTextures.size());
        m_rayTextures.push_back({binding.texture.id, t->view, sampler(binding.sampler)});
    }
    m_rayTextureSlots.emplace(key, slot);
    return slot;
}

void VulkanDevice::endRayScene() {
    if (m_pass != Pass::RayScene)
        return;
    m_pass = Pass::None;
    m_rayTop = m_rayScene.build(m_frames[m_frameIndex].cmd);
}

void VulkanDevice::bindRaySet(VkCommandBuffer cmd) {
    if (!m_rayTracing)
        return;
    const VkAccelerationStructureKHR top = m_rayTop ? m_rayTop : m_rayScene.empty();
    if (!top)
        return;
    VkDescriptorSet set = m_raySets[m_frameIndex];
    // The set is this frame's: the frame that used it last has finished.
    // It changes only between passes (the ray scene comes before them).
    if (m_rayWritten[m_frameIndex] != top) {
        VkWriteDescriptorSetAccelerationStructureKHR asw{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asw.accelerationStructureCount = 1;
        asw.pAccelerationStructures = &top;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.pNext = &asw;
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
        m_rayWritten[m_frameIndex] = top;
    }
    const VkBuffer table = m_rayScene.alphaTable(m_frameIndex);
    if (table && m_rayTableWritten[m_frameIndex] != table) {
        VkDescriptorBufferInfo bi{table, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 1;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
        m_rayTableWritten[m_frameIndex] = table;
    }
    if (!table)
        return;
    // The textures this set does not have yet, and those rewritten since.
    std::vector<std::uint32_t>& dirty = m_rayTexturesDirty[m_frameIndex];
    for (auto i = m_rayTexturesWritten[m_frameIndex]; i < m_rayTextures.size(); ++i)
        dirty.push_back(i);
    m_rayTexturesWritten[m_frameIndex] = static_cast<std::uint32_t>(m_rayTextures.size());
    if (!dirty.empty()) {
        std::vector<VkDescriptorImageInfo> infos;
        std::vector<VkWriteDescriptorSet> writes;
        infos.reserve(dirty.size());
        for (std::uint32_t slot : dirty) {
            const RayTexture& rt = m_rayTextures[slot];
            if (!rt.view)
                continue;
            infos.push_back({rt.sampler, rt.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = 2;
            w.dstArrayElement = slot;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes.push_back(w);
        }
        for (std::size_t i = 0; i < writes.size(); ++i)
            writes[i].pImageInfo = &infos[i];
        if (!writes.empty())
            vkUpdateDescriptorSets(m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        dirty.clear();
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 4, 1, &set, 0, nullptr);
}

void VulkanDevice::draw(const DrawCall& drawn) {
    if (drawn.count == 0 || m_pass == Pass::None)
        return;
    if (m_pass == Pass::RayScene) {
        captureRay(drawn);
        return;
    }
    // OpenMM2 sun shadows: the shadow pass draws casters depth only.
    std::optional<DrawCall> caster;
    if (m_pass == Pass::Shadow) {
        caster = shadowCasterCall(drawn);
        if (!caster)
            return;
    }
    const DrawCall& call = caster ? *caster : drawn;
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
    (void)drawn;

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

    if (call.state.vertexFormat == VertexFormat::Mesh)
        bindShadowSet(cmd);
    detail::GpuDrawConstants dc = detail::toGpu(call.constants);
    if (m_pass == Pass::Scene || m_pass == Pass::Probe)
        dc.flags = sceneFlags(call);
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
