#include "game/ModelLibrary.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/Trace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace mm2::game {
namespace {

std::uint32_t argbToRgba(std::uint32_t argb) {
    // 0xAARRGGBB -> 0xAABBGGRR (R in the lowest byte)
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
}

int lodRank(asset::Lod l) { return l == asset::Lod::None ? 0 : static_cast<int>(l); }

} // namespace

const GpuMesh* GpuModel::find(std::string_view part, asset::Lod lod) const {
    const GpuMesh* best = nullptr;
    int bestScore = 1 << 30;
    for (const auto& m : meshes) {
        if (!str::iequals(m.part, part))
            continue;
        const int d = lodRank(m.lod) - lodRank(lod);
        // Exact match first, then more detailed (d < 0), then less detailed.
        const int score = d == 0 ? 0 : (d < 0 ? 10 - d : 100 + d);
        if (score < bestScore) {
            bestScore = score;
            best = &m;
        }
    }
    return best;
}

const std::vector<asset::PkgMaterial>& GpuModel::materials(int paintjob) const {
    static const std::vector<asset::PkgMaterial> none;
    if (paintjobs.empty())
        return none;
    // vehCarModel::Init and lvlSky::Init take the paint job modulo the
    // number of shader sets.
    const auto i = static_cast<std::size_t>(std::max(paintjob, 0)) % paintjobs.size();
    return paintjobs[i];
}

ModelLibrary::ModelLibrary(render::Device& device, const vfs::Vfs& vfs) : m_device(device), m_vfs(vfs) {}

ModelLibrary::~ModelLibrary() {
    for (auto& [name, m] : m_models) {
        if (!m)
            continue;
        for (auto& mesh : m->meshes) {
            m_device.destroyBuffer(mesh.vertices);
            m_device.destroyBuffer(mesh.indices);
        }
    }
}

const GpuModel* ModelLibrary::get(std::string_view nameIn) {
    const std::string name = str::lower(nameIn);
    if (auto it = m_models.find(name); it != m_models.end())
        return it->second.get();
    const auto start = std::chrono::steady_clock::now();
    auto bytes = m_vfs.readAll("geometry/" + name + ".pkg");
    if (!bytes) {
        log::debug("model '{}' not found", name);
        m_models[name] = nullptr;
        return nullptr;
    }
    std::string error;
    auto pkg = asset::parsePkg(*bytes, &error);
    if (!pkg) {
        log::warn("model '{}': {}", name, error);
        m_models[name] = nullptr;
        return nullptr;
    }
    const GpuModel* model = add(name, *pkg);
    if (trace::active()) // OpenMM2 hitch log: a model loaded
        trace::note(std::format("model {} {:.1f} ms", name, trace::msSince(start)));
    return model;
}

const GpuModel* ModelLibrary::add(std::string_view nameIn, const asset::Pkg& pkg) {
    // A model is uploaded once per name; renderers keep pointers to it, so an
    // existing entry must never be replaced.
    if (auto it = m_models.find(str::lower(nameIn)); it != m_models.end() && it->second)
        return it->second.get();
    auto model = std::make_unique<GpuModel>();
    model->name = str::lower(nameIn);
    model->paintjobs = pkg.paintjobs;
    model->offset = pkg.offset;
    model->xrefs = pkg.xrefs;
    for (const auto& mesh : pkg.meshes) {
        GpuMesh gm;
        gm.part = mesh.part;
        gm.lod = mesh.lod;
        gm.bounds = mesh.bounds();
        std::vector<render::Vertex3D> vertices;
        std::vector<std::uint16_t> indices;
        for (const auto& section : mesh.sections) {
            for (const auto& packet : section.packets) {
                GpuMesh::Draw d;
                d.firstIndex = static_cast<std::uint32_t>(indices.size());
                d.indexCount = static_cast<std::uint32_t>(packet.indices.size());
                d.baseVertex = static_cast<std::int32_t>(vertices.size());
                d.shader = section.shaderIndex;
                for (const auto& v : packet.vertices) {
                    render::Vertex3D rv{};
                    rv.position[0] = v.position.x;
                    rv.position[1] = v.position.y;
                    rv.position[2] = v.position.z;
                    rv.normal[0] = v.normal.x;
                    rv.normal[1] = v.normal.y;
                    rv.normal[2] = v.normal.z;
                    rv.color = argbToRgba(v.color);
                    rv.uv0[0] = v.uv.x;
                    rv.uv0[1] = v.uv.y;
                    // gfxPacket::OrthoMap's cloud shadow coordinates, from the
                    // model-space position: ((y + x), (y + z)) / 128.
                    rv.uv1[0] = (v.position.y + v.position.x) * 0.0078125f;
                    rv.uv1[1] = (v.position.y + v.position.z) * 0.0078125f;
                    vertices.push_back(rv);
                }
                indices.insert(indices.end(), packet.indices.begin(), packet.indices.end());
                if (d.indexCount)
                    gm.draws.push_back(d);
            }
        }
        if (vertices.empty() || indices.empty())
            continue;
        gm.radius = mesh.radius();
        gm.vertices = m_device.createBuffer(render::BufferKind::Vertex, vertices.size() * sizeof(render::Vertex3D),
                                            vertices.data());
        gm.indices = m_device.createBuffer(render::BufferKind::Index, indices.size() * sizeof(std::uint16_t),
                                           indices.data());
        model->bounds.expand(gm.bounds);
        model->meshes.push_back(std::move(gm));
    }
    auto& slot = m_models[model->name] = std::move(model);
    return slot.get();
}

} // namespace mm2::game
