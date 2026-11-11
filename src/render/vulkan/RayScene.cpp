#include "render/vulkan/RayScene.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace mm2::render::vk {

std::size_t RayKeyHash::operator()(const RayKey& k) const {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t v : {k.vertexBuffer, k.vertexOffset, k.indexBuffer, k.indexOffset, k.first, k.count,
                            static_cast<std::uint32_t>(k.baseVertex), k.opaque ? 1u : 0u}) {
        h ^= v;
        h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
}

namespace {
VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
             VkAccessFlags dstAccess) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace

bool RayScene::init(VkDevice device, VmaAllocator allocator, VkPhysicalDevice gpu,
                    std::function<void(std::function<void()>)> defer) {
    m_device = device;
    m_allocator = allocator;
    m_defer = std::move(defer);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p.pNext = &as;
    vkGetPhysicalDeviceProperties2(gpu, &p);
    m_scratchAlign = std::max<VkDeviceSize>(as.minAccelerationStructureScratchOffsetAlignment, 1);
    return true;
}

void RayScene::destroy() {
    auto kill = [&](Accel& a) {
        if (a.as)
            vkDestroyAccelerationStructureKHR(m_device, a.as, nullptr);
        if (a.buffer)
            vmaDestroyBuffer(m_allocator, a.buffer, a.alloc);
        a = {};
    };
    for (auto& [k, a] : m_persistent)
        kill(a);
    m_persistent.clear();
    m_byBuffer.clear();
    for (auto& a : m_staticOwned)
        kill(a);
    m_staticOwned.clear();
    for (auto& a : m_backOwned)
        kill(a);
    m_backOwned.clear();
    for (const Kept& k : m_backKept)
        vmaDestroyBuffer(m_allocator, k.buffer, k.alloc);
    m_backKept.clear();
    m_backStatic.clear();
    m_backAlpha.clear();
    for (Slot& slot : m_slots) {
        for (Accel& a : slot.dynamic)
            if (a.as)
                vkDestroyAccelerationStructureKHR(m_device, a.as, nullptr);
        slot.dynamic.clear();
        for (Slab& r : slot.retired)
            freeSlab(r);
        slot.retired.clear();
        freeSlab(slot.slab);
        freeSlab(slot.scratch);
    }
    auto killTop = [&](TopLevel& t) {
        kill(t.accel);
        if (t.instances)
            vmaDestroyBuffer(m_allocator, t.instances, t.instancesAlloc);
        if (t.scratch)
            vmaDestroyBuffer(m_allocator, t.scratch, t.scratchAlloc);
        if (t.alpha)
            vmaDestroyBuffer(m_allocator, t.alpha, t.alphaAlloc);
        t = {};
    };
    for (auto& t : m_tops)
        killTop(t);
    killTop(m_empty);
    for (const Kept& k : m_kept)
        vmaDestroyBuffer(m_allocator, k.buffer, k.alloc);
    m_kept.clear();
    m_pending.clear();
    m_static.clear();
    m_dynamic.clear();
}

VkDeviceAddress RayScene::bufferAddress(VkBuffer b) const {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b;
    return vkGetBufferDeviceAddress(m_device, &i);
}

bool RayScene::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped, VkBuffer& buffer,
                            VmaAllocation& alloc, void** map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(size, 16);
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (mapped)
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bi, &ai, &buffer, &alloc, &info) != VK_SUCCESS) {
        log::error("vulkan: out of memory for a {} byte ray tracing buffer", size);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    if (map)
        *map = info.pMappedData;
    return true;
}

bool RayScene::createAccel(Accel& a, VkDeviceSize size, VkAccelerationStructureTypeKHR type) {
    if (!createBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, a.buffer, a.alloc, nullptr))
        return false;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = a.buffer;
    ci.size = size;
    ci.type = type;
    if (vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &a.as) != VK_SUCCESS) {
        vmaDestroyBuffer(m_allocator, a.buffer, a.alloc);
        a = {};
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = a.as;
    a.address = vkGetAccelerationStructureDeviceAddressKHR(m_device, &ai);
    a.size = size;
    return true;
}

void RayScene::freeSlab(Slab& s) {
    if (s.buffer)
        vmaDestroyBuffer(m_allocator, s.buffer, s.alloc);
    s = {};
}

// Grows `s` to hold `size` bytes. Its old buffer is the slot's own, which
// the GPU has finished with (or is kept by the caller).
bool RayScene::ensureSlab(Slab& s, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (s.buffer && s.size >= size)
        return true;
    freeSlab(s);
    const VkDeviceSize want = std::max<VkDeviceSize>(size, 4u << 20);
    if (!createBuffer(want, usage, false, s.buffer, s.alloc, nullptr))
        return false;
    s.size = want;
    s.address = bufferAddress(s.buffer);
    return true;
}

bool RayScene::createDynamicAccel(Accel& a, VkDeviceSize size) {
    Slot& slot = m_slots[m_slot];
    const VkDeviceSize offset = alignUp(slot.slab.used, 256); // structures start 256-byte aligned
    if (!slot.slab.buffer || offset + size > slot.slab.size) {
        // A larger slab; the full one still holds this frame's structures.
        if (slot.slab.buffer)
            slot.retired.push_back(slot.slab);
        slot.slab = {};
        if (!ensureSlab(slot.slab, std::max<VkDeviceSize>(size, slot.retired.empty() ? 0 : slot.retired.back().size * 2),
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR))
            return false;
        return createDynamicAccel(a, size);
    }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = slot.slab.buffer;
    ci.offset = offset;
    ci.size = size;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &a.as) != VK_SUCCESS)
        return false;
    slot.slab.used = offset + size;
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = a.as;
    a.address = vkGetAccelerationStructureDeviceAddressKHR(m_device, &ai);
    a.size = size;
    return true;
}

void RayScene::dropAccel(Accel& a) {
    if (!a.as && !a.buffer)
        return;
    const Accel copy = a;
    a = {};
    m_defer([this, copy] {
        if (copy.as)
            vkDestroyAccelerationStructureKHR(m_device, copy.as, nullptr);
        if (copy.buffer)
            vmaDestroyBuffer(m_allocator, copy.buffer, copy.alloc);
    });
}

VkAccelerationStructureGeometryKHR RayScene::triangles(const RayGeometry& g) const {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = g.opaque ? VkGeometryFlagsKHR(VK_GEOMETRY_OPAQUE_BIT_KHR) : VkGeometryFlagsKHR(0);
    auto& t = geo.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = g.vertices;
    t.vertexStride = g.stride;
    t.maxVertex = g.maxVertex;
    t.indexType = g.indices ? g.indexType : VK_INDEX_TYPE_NONE_KHR;
    t.indexData.deviceAddress = g.indices;
    return geo;
}

void RayScene::queueBottom(const RayGeometry& g, Accel& target, bool dynamic) {
    Pending p;
    p.geometry = g;
    p.target = &target;
    p.dynamic = dynamic;
    m_pending.push_back(p);
}

bool RayScene::continues(const RayGeometry& g, const RayGeometry& next) {
    if (g.vertices != next.vertices || g.indices != next.indices || g.stride != next.stride ||
        g.firstVertex != next.firstVertex || g.maxVertex != next.maxVertex)
        return false;
    const std::uint32_t unit = g.indices ? (g.indexType == VK_INDEX_TYPE_UINT16 ? 2u : 4u) : g.stride;
    return g.indexType == next.indexType && g.indexOffset + g.triangles * 3 * unit == next.indexOffset;
}

void RayScene::recordBottoms(VkCommandBuffer cmd) {
    if (m_pending.empty())
        return;
    // Size and create the structures (a failed one is left out: its
    // instances name no structure, which the top level skips).
    VkDeviceSize total = 0;
    for (Pending& p : m_pending) {
        const VkAccelerationStructureGeometryKHR geo = triangles(p.geometry);
        VkAccelerationStructureBuildGeometryInfoKHR info{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geo;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                                &p.geometry.triangles, &sizes);
        const bool ok = p.dynamic ? createDynamicAccel(*p.target, sizes.accelerationStructureSize)
                                  : createAccel(*p.target, sizes.accelerationStructureSize,
                                                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        p.scratch = ok ? sizes.buildScratchSize : 0;
        if (!ok)
            *p.target = {};
        total += alignUp(p.scratch, m_scratchAlign);
    }
    std::erase_if(m_pending, [](const Pending& p) { return !p.target->as; });
    if (m_pending.empty())
        return;
    Slab& scratch = m_slots[m_slot].scratch; // the slot's: its last builds are done
    if (!ensureSlab(scratch, total + m_scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        m_pending.clear();
        return;
    }
    const VkDeviceAddress base = alignUp(scratch.address, m_scratchAlign);
    std::vector<VkAccelerationStructureGeometryKHR> geos(m_pending.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(m_pending.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(m_pending.size());
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs(m_pending.size());
    VkDeviceSize offset = 0;
    for (std::size_t i = 0; i < m_pending.size(); ++i) {
        const Pending& p = m_pending[i];
        geos[i] = triangles(p.geometry);
        auto& info = infos[i];
        info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = p.target->as;
        info.geometryCount = 1;
        info.pGeometries = &geos[i];
        info.scratchData.deviceAddress = base + offset;
        offset += alignUp(p.scratch, m_scratchAlign);
        ranges[i] = {p.geometry.triangles, p.geometry.indexOffset, p.geometry.firstVertex, 0};
        rangePtrs[i] = &ranges[i];
    }
    // The geometry: per-frame memory the host wrote, meshes the uploads
    // copied.
    barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<std::uint32_t>(infos.size()), infos.data(),
                                        rangePtrs.data());
    barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    m_pending.clear();
}

void RayScene::dropBack() {
    for (Accel& a : m_backOwned)
        dropAccel(a);
    m_backOwned.clear();
    m_backStatic.clear();
    m_backAlpha.clear();
    VmaAllocator allocator = m_allocator;
    for (const Kept& k : m_backKept)
        m_defer([allocator, b = k.buffer, a = k.alloc] { vmaDestroyBuffer(allocator, b, a); });
    m_backKept.clear();
}

void RayScene::beginCapture(bool statics, std::uint32_t slot, bool first, bool last) {
    m_slot = slot % kSlots;
    m_capturingStatics = statics;
    m_partial = statics && !(first && last);
    m_finishPartial = m_partial && last;
    if (m_partial) {
        if (first)
            dropBack(); // a part capture left unfinished
    } else if (statics) {
        dropBack();
        m_staticsStale = false;
        for (Accel& a : m_staticOwned)
            dropAccel(a);
        m_staticOwned.clear();
        m_static.clear();
        m_staticAlpha.clear();
        VmaAllocator allocator = m_allocator;
        for (const Kept& k : m_kept)
            m_defer([allocator, b = k.buffer, a = k.alloc] { vmaDestroyBuffer(allocator, b, a); });
        m_kept.clear();
    }
    // The slot's last frame has finished: its dynamic structures can go now.
    Slot& s = m_slots[m_slot];
    for (Accel& a : s.dynamic)
        if (a.as)
            vkDestroyAccelerationStructureKHR(m_device, a.as, nullptr);
    s.dynamic.clear();
    for (Slab& r : s.retired)
        freeSlab(r);
    s.retired.clear();
    s.slab.used = 0;
    m_dynamic.clear();
    m_dynamicAlpha.clear();
    m_layer = Layer::Static;
}

void RayScene::add(const RayGeometry& g, const RayKey* key, const float world[4][4], const RayMaterial& material,
                   std::uint8_t mask, bool twoSided, bool flip) {
    if (m_layer == Layer::Static && !m_capturingStatics)
        return;
    if (g.triangles == 0 || !g.vertices)
        return;
    Instance inst;
    // The engine's row vectors (p' = p M) to Vulkan's 3 x 4 for column
    // vectors (p' = T p).
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            inst.transform[r][c] = world[c][r];
    const bool back = m_layer == Layer::Static && m_partial;
    auto& list = m_layer == Layer::Static ? (back ? m_backStatic : m_static) : m_dynamic;
    auto& materials = m_layer == Layer::Static ? (back ? m_backAlpha : m_staticAlpha) : m_dynamicAlpha;
    inst.mask = mask;
    inst.opaque = g.opaque;
    if (key) {
        auto it = m_persistent.find(*key);
        if (it == m_persistent.end()) {
            it = m_persistent.emplace(*key, Accel{}).first;
            queueBottom(g, it->second, false);
            m_byBuffer[key->vertexBuffer].push_back(*key);
            if (key->indexBuffer && key->indexBuffer != key->vertexBuffer)
                m_byBuffer[key->indexBuffer].push_back(*key);
        }
        inst.accel = &it->second;
    } else {
        const bool dynamic = m_layer == Layer::Dynamic;
        auto& owned = dynamic ? m_slots[m_slot].dynamic : back ? m_backOwned : m_staticOwned;
        // The next range of the last draw's buffers, placed the same (a
        // pedestrian's materials): its structure takes these triangles too.
        if (g.opaque && !owned.empty() && !m_pending.empty() && m_pending.back().target == &owned.back() &&
            !list.empty() && list.back().opaque &&
            list.back().accel == &owned.back() &&
            std::memcmp(list.back().transform, inst.transform, sizeof(inst.transform)) == 0 &&
            continues(m_pending.back().geometry, g)) {
            m_pending.back().geometry.triangles += g.triangles;
            return;
        }
        owned.emplace_back();
        queueBottom(g, owned.back(), dynamic);
        inst.accel = &owned.back();
    }
    inst.material = static_cast<std::int32_t>(materials.size());
    materials.push_back(material);
    if (!twoSided && (mask & kRayReflect) != 0 && (mask & kRayShadow) != 0) {
        // One instance each: both faces for the shadows, the front for the
        // reflections.
        Instance shadow = inst;
        shadow.mask = kRayShadow;
        list.push_back(shadow);
        inst.mask = kRayReflect;
    }
    inst.twoSided = twoSided || (inst.mask & kRayReflect) == 0;
    inst.flip = flip;
    list.push_back(inst);
}

VkDeviceAddress RayScene::keepStatic(const void* data, std::size_t bytes) {
    std::vector<Kept>& kept = m_partial ? m_backKept : m_kept;
    const VkDeviceSize size = alignUp(std::max<VkDeviceSize>(bytes, 4), 16);
    if (kept.empty() || kept.back().used + size > kept.back().size) {
        Kept k;
        k.size = std::max<VkDeviceSize>(size, 4u << 20);
        void* map = nullptr;
        if (!createBuffer(k.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, k.buffer, k.alloc, &map))
            return 0;
        k.mapped = static_cast<std::uint8_t*>(map);
        k.address = bufferAddress(k.buffer);
        kept.push_back(k);
    }
    Kept& k = kept.back();
    std::memcpy(k.mapped + k.used, data, bytes);
    const VkDeviceAddress at = k.address + k.used;
    k.used += size;
    return at;
}

void RayScene::forgetBuffer(std::uint32_t bufferId) {
    auto it = m_byBuffer.find(bufferId);
    if (it == m_byBuffer.end())
        return;
    for (const RayKey& k : it->second) {
        if (auto p = m_persistent.find(k); p != m_persistent.end()) {
            // The kept static layer may name it: its instances go, and the
            // layer is captured anew.
            const Accel* gone = &p->second;
            const auto before = m_static.size();
            std::erase_if(m_static, [gone](const Instance& i) { return i.accel == gone; });
            std::erase_if(m_backStatic, [gone](const Instance& i) { return i.accel == gone; });
            std::erase_if(m_dynamic, [gone](const Instance& i) { return i.accel == gone; });
            std::erase_if(m_pending, [gone](const Pending& q) { return q.target == gone; });
            if (m_static.size() != before)
                m_staticsStale = true;
            dropAccel(p->second);
            m_persistent.erase(p);
        }
    }
    m_byBuffer.erase(it);
}

bool RayScene::ensureTop(TopLevel& t, std::uint32_t count) {
    count = std::max(count, 1u);
    if (t.accel.as && t.capacity >= count)
        return true;
    std::uint32_t capacity = std::max(t.capacity, 256u);
    while (capacity < count)
        capacity *= 2;
    dropAccel(t.accel);
    VmaAllocator allocator = m_allocator;
    if (t.instances) {
        m_defer([allocator, b = t.instances, a = t.instancesAlloc] { vmaDestroyBuffer(allocator, b, a); });
        t.instances = VK_NULL_HANDLE;
    }
    if (t.scratch) {
        m_defer([allocator, b = t.scratch, a = t.scratchAlloc] { vmaDestroyBuffer(allocator, b, a); });
        t.scratch = VK_NULL_HANDLE;
    }
    if (!createBuffer(sizeof(VkAccelerationStructureInstanceKHR) * capacity,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, t.instances,
                      t.instancesAlloc, &t.instancesMapped))
        return false;
    t.instancesAddress = bufferAddress(t.instances);
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.data.deviceAddress = t.instancesAddress;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                            &capacity, &sizes);
    if (!createAccel(t.accel, sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR))
        return false;
    if (!createBuffer(sizes.buildScratchSize + m_scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, t.scratch,
                      t.scratchAlloc, nullptr))
        return false;
    t.scratchAddress = alignUp(bufferAddress(t.scratch), m_scratchAlign);
    t.capacity = capacity;
    return true;
}

namespace {
void recordTop(VkCommandBuffer cmd, VkAccelerationStructureKHR dst, VkDeviceAddress instances,
               VkDeviceAddress scratch, std::uint32_t count) {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.data.deviceAddress = instances;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.dstAccelerationStructure = dst;
    info.geometryCount = 1;
    info.pGeometries = &geo;
    info.scratchData.deviceAddress = scratch;
    const VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ranges);
    // For the scene's (and the probe's) fragment shaders.
    barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}
} // namespace

void RayScene::buildEmpty(VkCommandBuffer cmd) {
    if (m_empty.accel.as)
        return;
    if (!ensureTop(m_empty, 1))
        return;
    if (createBuffer(sizeof(RayMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, m_empty.alpha, m_empty.alphaAlloc,
                     &m_empty.alphaMapped))
        m_empty.alphaCapacity = 1;
    recordTop(cmd, m_empty.accel.as, m_empty.instancesAddress, m_empty.scratchAddress, 0);
}

VkAccelerationStructureKHR RayScene::build(VkCommandBuffer cmd) {
    buildEmpty(cmd);
    recordBottoms(cmd);
    if (m_finishPartial) {
        // The last part: the back layer (its structures built by now, this
        // one's above) replaces the traced one, which goes once its frames
        // are done.
        std::swap(m_staticOwned, m_backOwned);
        std::swap(m_static, m_backStatic);
        std::swap(m_staticAlpha, m_backAlpha);
        std::swap(m_kept, m_backKept);
        dropBack();
        m_staticsStale = false;
        m_finishPartial = false;
        m_partial = false;
    }
    TopLevel& t = m_tops[m_slot];
    const auto count = static_cast<std::uint32_t>(m_static.size() + m_dynamic.size());
    if (!ensureTop(t, count))
        return m_empty.accel.as;
    // Built here and copied at once: the mapped memory is write-combined,
    // and the instance's bit fields would read it back.
    m_instanceData.resize(count);
    std::uint32_t n = 0;
    const auto staticAlphas = static_cast<std::int32_t>(m_staticAlpha.size());
    for (const auto* list : {&m_static, &m_dynamic})
        for (const Instance& i : *list) {
            VkAccelerationStructureInstanceKHR& v = m_instanceData[n++];
            std::memcpy(&v.transform, i.transform, sizeof(v.transform));
            v.instanceCustomIndex = static_cast<std::uint32_t>(i.material + (list == &m_dynamic ? staticAlphas : 0));
            v.mask = i.mask;
            v.instanceShaderBindingTableRecordOffset = 0;
            // Both faces cast, as in the shadow maps.
            VkGeometryInstanceFlagsKHR flags = 0;
            if (i.twoSided)
                flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            if (i.flip)
                flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            if (i.opaque)
                flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            v.flags = flags;
            v.accelerationStructureReference = i.accel ? i.accel->address : 0; // 0: inactive
        }
    if (count)
        std::memcpy(t.instancesMapped, m_instanceData.data(), sizeof(VkAccelerationStructureInstanceKHR) * count);
    // The material table: the static layer's entries, then the dynamic one's.
    const auto alphas = static_cast<std::uint32_t>(m_staticAlpha.size() + m_dynamicAlpha.size());
    if (!t.alpha || t.alphaCapacity < std::max(alphas, 1u)) {
        VmaAllocator allocator = m_allocator;
        if (t.alpha)
            m_defer([allocator, b = t.alpha, a = t.alphaAlloc] { vmaDestroyBuffer(allocator, b, a); });
        t.alphaCapacity = std::max(t.alphaCapacity * 2, std::max(alphas, 256u));
        if (!createBuffer(sizeof(RayMaterial) * t.alphaCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, t.alpha,
                          t.alphaAlloc, &t.alphaMapped)) {
            t.alpha = VK_NULL_HANDLE;
            t.alphaCapacity = 0;
        }
    }
    if (t.alpha && !m_staticAlpha.empty())
        std::memcpy(t.alphaMapped, m_staticAlpha.data(), sizeof(RayMaterial) * m_staticAlpha.size());
    if (t.alpha && !m_dynamicAlpha.empty())
        std::memcpy(static_cast<RayMaterial*>(t.alphaMapped) + m_staticAlpha.size(), m_dynamicAlpha.data(),
                    sizeof(RayMaterial) * m_dynamicAlpha.size());
    recordTop(cmd, t.accel.as, t.instancesAddress, t.scratchAddress, count);
    return t.accel.as;
}

} // namespace mm2::render::vk
