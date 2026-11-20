#pragma once

#include "asset/Pkg.h"
#include "render/Device.h"
#include "vfs/Vfs.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mm2::game {

// One PKG geometry chunk on the GPU: a vertex buffer for all its packets, a
// 16-bit index buffer, and one draw per packet (packets keep their own
// vertex ranges through baseVertex).
struct GpuMesh {
    std::string part; // "BODY", "WHL0", "" for plain LOD meshes
    asset::Lod lod = asset::Lod::None;
    render::BufferHandle vertices;
    render::BufferHandle indices;
    struct Draw {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::int32_t baseVertex = 0;
        std::uint32_t shader = 0; // material index within a paint job
    };
    std::vector<Draw> draws;
    Aabb bounds;
    // modGetStatic's radius: the largest distance of a vertex from the
    // model's origin.
    float radius = 0.0f;
};

struct GpuModel {
    std::string name;
    std::vector<GpuMesh> meshes;
    std::vector<std::vector<asset::PkgMaterial>> paintjobs;
    std::optional<Vec3> offset;
    std::vector<asset::PkgXref> xrefs;
    Aabb bounds; // all meshes

    // Best available LOD of `part`, preferring `lod`, then more detailed ones,
    // then less detailed ones.
    const GpuMesh* find(std::string_view part, asset::Lod lod) const;
    const std::vector<asset::PkgMaterial>& materials(int paintjob) const;
};

// Loads geometry/<name>.pkg on demand and keeps it on the GPU.
class ModelLibrary {
public:
    ModelLibrary(render::Device& device, const vfs::Vfs& vfs);
    ~ModelLibrary();
    ModelLibrary(const ModelLibrary&) = delete;
    ModelLibrary& operator=(const ModelLibrary&) = delete;

    // Null when the model does not exist or cannot be parsed (logged once).
    const GpuModel* get(std::string_view name);
    // Uploads an already parsed package under `name`, or returns the model
    // already uploaded under that name (pointers stay valid for the
    // library's lifetime).
    const GpuModel* add(std::string_view name, const asset::Pkg& pkg);

    const vfs::Vfs& vfs() const { return m_vfs; }

private:
    render::Device& m_device;
    const vfs::Vfs& m_vfs;
    std::unordered_map<std::string, std::unique_ptr<GpuModel>> m_models; // null = missing
};

} // namespace mm2::game
