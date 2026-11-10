#pragma once

// OpenMM2 ray traced sun shadows (not in MM2; Vulkan with VK_KHR_ray_query):
// the scene rays are traced against. The device captures draw calls into it
// (VulkanDevice's ray scene pass) and this builds their bottom level
// acceleration structures and, every frame, the top level one over them.
//
// Three kinds of bottom level structure:
//  * persistent: a range of a mesh's own (non transient) buffers, built the
//    first time it is seen and kept until one of its buffers is destroyed;
//  * static: captured into the static layer from per-frame memory (the
//    city's street geometry, whose indices are uploaded per frame), kept
//    until the static layer is captured again;
//  * dynamic: captured into the dynamic layer from per-frame memory (the
//    skinned pedestrians), built every frame into a slab of memory per
//    frame in flight, which the next capture in that slot reuses.
// The static layer's instances are kept between captures; the dynamic
// layer's are captured every frame. Every instance names an entry of the
// material table (RayMaterial, as the shaders read it): where its vertices
// are, its texture and colour, through which the shaders find the alpha of
// an alpha tested draw (trees, fences: not opaque) where a ray meets it, and
// the colour a reflection ray sees. Its mask tells the rays that see it
// (kRayShadow: the sun's; kRayReflect: the reflections').

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace mm2::render::vk {

// One draw's triangles, as the device found them.
struct RayGeometry {
    VkDeviceAddress vertices = 0; // the first vertex's position (float3 at offset 0)
    VkDeviceAddress indices = 0;  // 0: not indexed
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    std::uint32_t stride = 0;         // bytes between vertices
    std::uint32_t maxVertex = 0;      // the highest vertex the indices may name
    std::uint32_t triangles = 0;
    std::uint32_t indexOffset = 0;    // bytes into the indices (indexed) or first vertex (not)
    std::uint32_t firstVertex = 0;    // added to every index
    bool opaque = true;               // false: alpha tested (its material says how)
};

// Which rays see an instance (its mask).
inline constexpr std::uint8_t kRayShadow = 0x01;
inline constexpr std::uint8_t kRayReflect = 0x02;
// The sky dome, which the game draws around the camera with no depth: not
// in the way of anything, only looked up by the reflections whose rays
// met nothing, from the camera (the dynamic layer has it there).
inline constexpr std::uint8_t kRaySky = 0x04;

// How the shaders find a triangle's texture coordinates, colour and normal
// (mesh.frag's RayMaterial; std430, 64 bytes).
struct RayMaterial {
    static constexpr std::uint32_t kAlphaTest = 1;   // solid only where the texture's alpha is above alphaRef
    static constexpr std::uint32_t kVertexColor = 2; // times the vertex colour (pre-lit city geometry)
    static constexpr std::uint32_t kLighting = 4;    // lit by the frame's lights with its normals
    static constexpr std::uint32_t kSky = 8;         // the sky dome: no fog
    static constexpr std::uint32_t kGlow = 16;       // added along the ray, blocking nothing (lights' glows)
    VkDeviceAddress vertices = 0;   // vertex 0 (Vertex3D: normal at byte 12, colour 24, uv0 28 of 44)
    VkDeviceAddress indices = 0;    // index 0; 0: not indexed
    std::uint32_t indexBytes = 0;   // 2, 4, or 0: not indexed
    std::uint32_t firstVertex = 0;  // added to every index
    std::uint32_t firstIndex = 0;   // the geometry's first index (or vertex)
    std::uint32_t texture = 0;      // in the shaders' texture array (0: white)
    std::uint32_t flags = 0;
    float alphaRef = 0.0f;
    std::uint32_t pad[2]{};
    float color[4] = {1, 1, 1, 1};  // the draw's colour
};
static_assert(sizeof(RayMaterial) == 64);

// What makes a persistent geometry the same one again.
struct RayKey {
    std::uint32_t vertexBuffer = 0, vertexOffset = 0;
    std::uint32_t indexBuffer = 0, indexOffset = 0;
    std::uint32_t first = 0, count = 0;
    std::int32_t baseVertex = 0;
    bool opaque = true;
    bool operator==(const RayKey&) const = default;
};
struct RayKeyHash {
    std::size_t operator()(const RayKey& k) const;
};

class RayScene {
public:
    static constexpr std::uint32_t kSlots = 2; // frames in flight: one top level structure each

    // `defer` runs a deletion once the frames that may use the object are
    // done (VulkanDevice::deferDelete).
    bool init(VkDevice device, VmaAllocator allocator, VkPhysicalDevice gpu,
              std::function<void(std::function<void()>)> defer);
    void destroy();

    enum class Layer { Static, Dynamic };
    // Starts a capture for frame slot `slot` (whose last frame has
    // finished): with `statics`, the static layer is captured anew (its
    // instances and static structures dropped); without, it is kept.
    void beginCapture(bool statics, std::uint32_t slot);
    void setLayer(Layer layer) { m_layer = layer; }
    Layer layer() const { return m_layer; }
    bool capturingStatics() const { return m_capturingStatics; }
    // A captured draw: `key` for persistent geometry, null for per-frame.
    // `world`: the engine's row-vector matrix (rows: x, y, z axes, position).
    // `mask`: the rays that see it (kRayShadow, kRayReflect). The shadow
    // rays see both faces (as the shadow maps); the reflections' only the
    // front of a one-sided draw (`twoSided` false; `flip`: its front faces
    // wind the other way), as the game shows it: so they do not meet the
    // undersides of roofs and canopies the game never draws.
    void add(const RayGeometry& g, const RayKey* key, const float world[4][4], const RayMaterial& material,
             std::uint8_t mask, bool twoSided = true, bool flip = false);
    // Copies `bytes` into memory the static layer keeps until it is
    // captured again, and returns its address (0: out of memory): what the
    // shaders read of a static draw must outlive its per-frame memory.
    VkDeviceAddress keepStatic(const void* data, std::size_t bytes);
    // A buffer is going away: its persistent structures with it.
    void forgetBuffer(std::uint32_t bufferId);

    // Records the builds (outside any render pass) and returns the
    // capture's slot's top level structure.
    VkAccelerationStructureKHR build(VkCommandBuffer cmd);
    // A top level structure with no instances, for the frames without a
    // capture (the shaders' descriptor must name one). Built by the first
    // build() or buildEmpty().
    VkAccelerationStructureKHR empty() const { return m_empty.accel.as; }
    void buildEmpty(VkCommandBuffer cmd);

    std::size_t instanceCount() const { return m_static.size() + m_dynamic.size(); }
    // Slot `slot`'s material table (the shaders' set 4 binding 1); it
    // changes when it had to grow.
    // (The empty structure's one-entry table before any build.)
    VkBuffer alphaTable(std::uint32_t slot) const {
        return m_tops[slot % kSlots].alpha ? m_tops[slot % kSlots].alpha : m_empty.alpha;
    }
    // A persistent structure the kept static layer used has gone (its mesh
    // was destroyed): the static layer must be captured anew.
    bool staticsStale() const { return m_staticsStale || m_static.empty(); }

private:
    struct Accel {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
    };
    // A bottom level structure to build: created (sized) when the builds
    // are recorded, so that draws of one mesh in a row can join it first.
    struct Pending {
        RayGeometry geometry;
        Accel* target = nullptr;
        bool dynamic = false;
        VkDeviceSize scratch = 0;
    };
    struct Instance {
        const Accel* accel = nullptr; // its address once built (0: none)
        float transform[3][4];
        std::int32_t material = 0;    // in its layer's materials
        std::uint8_t mask = kRayShadow | kRayReflect;
        bool opaque = true;
        bool twoSided = true;
        bool flip = false;
    };
    struct TopLevel {
        Accel accel;
        VkBuffer instances = VK_NULL_HANDLE;
        VmaAllocation instancesAlloc = nullptr;
        void* instancesMapped = nullptr;
        VkDeviceAddress instancesAddress = 0;
        VkBuffer scratch = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = nullptr;
        VkDeviceAddress scratchAddress = 0;
        std::uint32_t capacity = 0;
        VkBuffer alpha = VK_NULL_HANDLE; // the material table
        VmaAllocation alphaAlloc = nullptr;
        void* alphaMapped = nullptr;
        std::uint32_t alphaCapacity = 0;
    };

    // A buffer sub-allocated by hand: the dynamic structures' slab, scratch.
    struct Slab {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0, used = 0;
    };
    // Per frame in flight: the dynamic structures (on the slab, which a
    // larger one replaces when full: the old waits in `retired`) and the
    // build scratch.
    struct Slot {
        Slab slab, scratch;
        std::vector<Slab> retired;
        std::deque<Accel> dynamic; // their buffer is the slab's: not owned
    };

    bool createAccel(Accel& a, VkDeviceSize size, VkAccelerationStructureTypeKHR type);
    bool createDynamicAccel(Accel& a, VkDeviceSize size);
    bool ensureSlab(Slab& s, VkDeviceSize size, VkBufferUsageFlags usage);
    void freeSlab(Slab& s);
    void dropAccel(Accel& a); // deferred
    VkDeviceAddress bufferAddress(VkBuffer b) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped, VkBuffer& buffer,
                      VmaAllocation& alloc, void** map);
    VkAccelerationStructureGeometryKHR triangles(const RayGeometry& g) const;
    // Queues a bottom level build for `g` into `target`.
    void queueBottom(const RayGeometry& g, Accel& target, bool dynamic);
    // Whether `next` continues `g`'s triangles (the next index range of the
    // same buffers), so one structure can hold both.
    static bool continues(const RayGeometry& g, const RayGeometry& next);
    void recordBottoms(VkCommandBuffer cmd);
    bool ensureTop(TopLevel& t, std::uint32_t count);

    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    std::function<void(std::function<void()>)> m_defer;
    VkDeviceSize m_scratchAlign = 128;

    std::unordered_map<RayKey, Accel, RayKeyHash> m_persistent;
    std::unordered_map<std::uint32_t, std::vector<RayKey>> m_byBuffer; // buffer id -> its persistent keys
    std::deque<Accel> m_staticOwned; // the static layer's structures from per-frame memory (stable addresses)
    std::array<Slot, kSlots> m_slots{};
    std::uint32_t m_slot = 0;
    std::vector<Instance> m_static, m_dynamic;
    std::vector<RayMaterial> m_staticAlpha, m_dynamicAlpha; // the layers' materials
    struct Kept {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        std::uint8_t* mapped = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0, used = 0;
    };
    std::vector<Kept> m_kept; // keepStatic's memory
    std::vector<Pending> m_pending;
    std::vector<VkAccelerationStructureInstanceKHR> m_instanceData;
    Layer m_layer = Layer::Static;
    bool m_capturingStatics = false;
    bool m_staticsStale = true;
    std::array<TopLevel, kSlots> m_tops{};
    TopLevel m_empty;
};

} // namespace mm2::render::vk
