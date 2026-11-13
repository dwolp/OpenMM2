#include "game/Mm1CityRenderer.h"

#include "asset/Bms.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/CityRenderer.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::game {
namespace {

// Inferred from Open1560's StaticTerrainLodTable (cellrend.cpp; its use is
// not yet ported): a cell's high meshes out to 150 m, medium to 300 m, low
// beyond.
constexpr float kCellHigh = 150.0f;
constexpr float kCellMedium = 300.0f;
// BuildingMaxDist (cellrend.cpp): facades are not drawn beyond it.
constexpr float kBuildingMaxDist = 1000.0f;

// mmInstance::GetMeshSetSet's names for a group's levels.
constexpr std::array<std::string_view, 4> kLodSuffix{"h", "m", "l", "vl"};

} // namespace

Mm1CityRenderer::Mm1CityRenderer(render::Device& device, TextureLibrary& textures, ModelLibrary& models,
                                 const vfs::Vfs& vfs, const city::Mm1City& city)
    : m_device(device), m_textures(textures), m_models(models), m_vfs(vfs), m_city(city) {
    if (auto b = m_vfs.readAll(std::string(vfs::kMm1Prefix) + "mtl/global.tsh")) {
        std::string err;
        m_sheet = asset::Mm1TexSheet::parse(std::string_view(reinterpret_cast<const char*>(b->data()), b->size()),
                                            &err);
        if (!m_sheet)
            log::warn("mm1: mtl/global.tsh: {}", err);
    }

    // The cells (mmCellRenderer::Init). CULL<n> or CULL<n>_H is required.
    for (const auto& c : city.cells) {
        Cell cell;
        cell.index = c.index;
        Aabb box;
        for (std::size_t g = 0; g < city::kMm1CellGroups.size(); ++g) {
            Mesh* m = mesh(city.cellMeshPath(c.index, city::kMm1CellGroups[g]));
            if (!m && g == 0)
                m = mesh(std::format("{}bms/{}/cull{:02}.bms", vfs::kMm1Prefix, city.cellMeshDir(c.index), c.index));
            cell.groups[g] = m;
            if (m)
                box.expand(m->model->bounds);
        }
        if (!box.valid())
            continue;
        cell.centre = (box.min + box.max) * 0.5f;
        cell.radius = (box.max - box.min).mag() * 0.5f;
        m_cells.push_back(cell);
    }

    // The facades and buildings (mmCullCity::AddInstance, InitFacade,
    // mmBuildingInstance::Init). Shear instances are placed as facades
    // (inferred until mmShearInstance is ported).
    using namespace city::Mm1InstFlag;
    for (const auto& f : city.facades) {
        if (!(f.flags & Static))
            continue;
        Facade fac;
        const std::string name = str::lower(f.name);
        std::vector<std::string_view> groups;
        if (!(f.flags & Shear) && (f.flags & Building)) {
            fac.transform = city::mm1BuildingMatrix(f.start, f.end, f.sides);
            groups = {"facade", "grnd"};
        } else {
            fac.transform = city::mm1MatrixFromPoints(f.start, f.end, f.width);
            groups = {"facade", "grnd"};
            if (f.flags & FacadeTop)
                groups.push_back("top");
            if (f.flags & FacadeLeft)
                groups.push_back("left");
            if (f.flags & FacadeRight)
                groups.push_back("right");
            if (f.flags & FacadeBack)
                groups.push_back("back");
        }
        fac.world = Mat44::fromMat34(fac.transform);
        Aabb box;
        for (auto g : groups) {
            std::array<Mesh*, 4> lods{};
            bool any = false;
            auto fill = [&](std::string_view group) {
                for (std::size_t l = 0; l < kLodSuffix.size(); ++l) {
                    if (lods[l])
                        continue;
                    lods[l] = mesh(std::format("{}bms/{}/{}_{}.bms", vfs::kMm1Prefix, name, group, kLodSuffix[l]));
                    any = any || lods[l];
                }
                if (!lods[0] && !any)
                    if ((lods[0] = mesh(std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, name, group))))
                        any = true;
            };
            fill(g);
            if (!any && g == "facade") {
                fill("bldg");
                // GetMeshSetSet's fallback: plain H, M, L, VL.
                if (!any)
                    for (std::size_t l = 0; l < kLodSuffix.size(); ++l)
                        if ((lods[l] = mesh(std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, name, kLodSuffix[l]))))
                            any = true;
            }
            if (!any)
                continue;
            // A missing level takes the next less detailed one, as MM2's
            // lvlInstance::GetGeomSet does (MM1's own rule is unported).
            for (int l = 2; l >= 0; --l)
                if (!lods[static_cast<std::size_t>(l)])
                    lods[static_cast<std::size_t>(l)] = lods[static_cast<std::size_t>(l) + 1];
            for (auto* m : lods)
                if (m) {
                    box.expand(fac.transform.transform(m->model->bounds.min));
                    box.expand(fac.transform.transform(m->model->bounds.max));
                }
            fac.groups.push_back(lods);
        }
        if (fac.groups.empty() || !box.valid())
            continue;
        fac.centre = (box.min + box.max) * 0.5f;
        fac.radius = (box.max - box.min).mag() * 0.5f;
        m_facades.push_back(std::move(fac));
    }

    // The drawbridges' leaves and gates (city/<city>.gizmo), at the gizmo
    // file's frames until game::world::Mm1Bridges moves them
    // (setBridgePart). They draw as facades do: plain H/M/L/VL meshes, plus
    // the leaves' RAIL group.
    for (const auto& set : city.bridges) {
        auto& indices = m_bridgeFacades.emplace_back();
        for (const auto& part : set.parts) {
            indices.push_back(-1);
            Facade fac;
            fac.transform = part.matrix();
            fac.world = Mat44::fromMat34(fac.transform);
            Aabb box;
            for (std::string_view group : {std::string_view{}, std::string_view{"rail"}}) {
                if (!group.empty() && !part.leaf())
                    continue;
                std::array<Mesh*, 4> lods{};
                for (std::size_t l = 0; l < kLodSuffix.size(); ++l)
                    lods[l] = mesh(group.empty() ? std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, part.model,
                                                               kLodSuffix[l])
                                                 : std::format("{}bms/{}/{}_{}.bms", vfs::kMm1Prefix, part.model,
                                                               group, kLodSuffix[l]));
                for (int l = 2; l >= 0; --l)
                    if (!lods[static_cast<std::size_t>(l)])
                        lods[static_cast<std::size_t>(l)] = lods[static_cast<std::size_t>(l) + 1];
                if (!lods[0])
                    continue;
                for (auto* m : lods)
                    if (m) {
                        box.expand(fac.transform.transform(m->model->bounds.min));
                        box.expand(fac.transform.transform(m->model->bounds.max));
                    }
                fac.groups.push_back(lods);
            }
            if (fac.groups.empty() || !box.valid())
                continue;
            fac.centre = (box.min + box.max) * 0.5f;
            // A leaf turns about its hinge: the radius about the hinge covers
            // every angle.
            fac.radius = std::max((box.max - box.min).mag() * 0.5f, (box.max - fac.transform.m3).mag());
            fac.radius = std::max(fac.radius, (box.min - fac.transform.m3).mag());
            fac.localCentre = fac.transform.inverse().transform(fac.centre);
            indices.back() = static_cast<int>(m_facades.size());
            m_facades.push_back(std::move(fac));
        }
    }

    m_sky = mesh(std::string(vfs::kMm1Prefix) + "bms/mmsky.bms");

    // OpenMM2 water on MM1's water (mmCellRenderer::Init singles out T_WATER).
    m_textures.setWaterTextures({std::string(kMm1TexturePrefix) + "t_water"});
    log::info("mm1: {} cells, {} facades, {} meshes ({} missing)", m_cells.size(), m_facades.size(), m_meshes.size(),
              m_missing);
}

void Mm1CityRenderer::setBridgePart(std::size_t set, std::size_t part, const Mat34& transform) {
    if (set >= m_bridgeFacades.size() || part >= m_bridgeFacades[set].size())
        return;
    const int i = m_bridgeFacades[set][part];
    if (i < 0)
        return;
    Facade& fac = m_facades[static_cast<std::size_t>(i)];
    fac.transform = transform;
    fac.world = Mat44::fromMat34(transform);
    fac.centre = transform.transform(fac.localCentre);
}

Mm1CityRenderer::Mesh* Mm1CityRenderer::mesh(const std::string& path) {
    if (auto it = m_meshes.find(path); it != m_meshes.end())
        return it->second.get();
    std::unique_ptr<Mesh> out;
    if (auto bytes = m_vfs.readAll(path)) {
        std::string err;
        if (auto bms = asset::parseBms(*bytes, &err)) {
            auto converted = asset::bmsToPkgMesh(*bms);
            asset::Pkg pkg;
            pkg.meshes.push_back(std::move(converted.mesh));
            pkg.paintjobs.push_back(converted.materials);
            if (const GpuModel* model = m_models.add("mm1:" + path, pkg); model && !model->meshes.empty()) {
                out = std::make_unique<Mesh>();
                out->model = model;
                for (const auto& m : converted.materials)
                    out->textures.push_back(m.texture);
                out->materials = converted.materials;
                out->centre = (model->bounds.min + model->bounds.max) * 0.5f;
                out->radius = (model->bounds.max - model->bounds.min).mag() * 0.5f;
            }
        } else {
            log::warn("mm1: {}: {}", path, err);
        }
    }
    if (!out)
        ++m_missing;
    Mesh* raw = out.get();
    m_meshes.emplace(path, std::move(out));
    return raw;
}

void Mm1CityRenderer::resolveMaterials(bool night) {
    // GetPackedTexture: the sheet's name for the look (TextureSuffix and
    // the night alternates); names the sheet lacks are used as they are.
    for (auto& [path, m] : m_meshes) {
        if (!m)
            continue;
        for (std::size_t i = 0; i < m->textures.size(); ++i) {
            const std::string& raw = m->textures[i];
            if (raw.empty())
                continue;
            std::string resolved =
                m_sheet ? m_sheet->resolve(raw, 0, night, m_textures.rain() ? asset::Mm1TexWeather::Rain
                                                                           : asset::Mm1TexWeather::Clear)
                        : std::string();
            if (resolved.empty())
                resolved = raw;
            m->materials[i].texture = std::string(kMm1TexturePrefix) + str::lower(resolved);
        }
    }
    m_night = night;
}

void Mm1CityRenderer::drawSky(const Camera& camera, int skyIndex, int& drawCalls) {
    if (!m_sky || skyIndex < 0)
        return;
    static constexpr std::array<char, 4> kTimes{'m', 'n', 's', 'd'};
    static constexpr std::array<char, 4> kWeathers{'c', 'c', 'f', 'r'}; // MM2's clear, cloudy, fog, rain
    const std::string texture = std::format("{}sky_{}{}", kMm1TexturePrefix, kTimes[static_cast<std::size_t>(skyIndex / 4) % 4],
                                            kWeathers[static_cast<std::size_t>(skyIndex % 4)]);
    const WorldTexture* tex = m_textures.get(texture);
    const Vec3 eye = camera.position();
    const Mat44 world = Mat44::fromMat34(Mat34::translation(eye));
    for (const auto& mesh : m_sky->model->meshes)
        for (const auto& d : mesh.draws) {
            render::DrawCall call;
            call.vertices = {mesh.vertices, 0};
            call.indices = {mesh.indices, 0};
            call.count = d.indexCount;
            call.first = d.firstIndex;
            call.baseVertex = d.baseVertex;
            call.constants.world = world;
            // As MM2's lvlSky: never in the sun shadows.
            call.constants.flags = render::DrawFlag::VertexColor | render::DrawFlag::NoShadow;
            if (tex) {
                call.constants.flags |= render::DrawFlag::Texture0;
                call.textures[0] = {tex->handle, tex->sampler};
            }
            call.state.depthTest = false;
            call.state.depthWrite = false;
            call.state.cull = render::CullMode::None;
            m_device.draw(call);
            ++drawCalls;
        }
}

void Mm1CityRenderer::drawMesh(Mesh& m, const Mat44& world, Stats& stats) {
    MeshDrawOptions options;
    // Inferred until MM1's winding is confirmed in game: both faces.
    options.cull = false;
    for (const auto& gm : m.model->meshes)
        stats.drawCalls += drawGpuMesh(m_device, m_textures, gm, m.materials, world, options);
}

void Mm1CityRenderer::draw(const Camera& camera, const Frustum& frustum, const DetailSettings& detail,
                           float farClip, Stats& stats) {
    if (!m_night || *m_night != m_textures.night())
        resolveMaterials(m_textures.night());
    const Vec3 eye = camera.position();
    const Mat44 identity = Mat44::fromMat34(Mat34::identity());
    const bool full = detail.fullDetail || detail.streetsFull;

    for (auto& cell : m_cells) {
        if (!frustum.intersectsSphere(cell.centre, cell.radius))
            continue;
        const float d = std::max(0.0f, (cell.centre - eye).mag() - cell.radius);
        if (d > farClip)
            continue;
        ++stats.cellsDrawn;
        // Each set: its level for the distance, then its "A" group.
        const std::size_t lod = full ? 0 : (d < kCellHigh ? 0 : (d < kCellMedium ? 1 : 2));
        for (std::size_t set : {std::size_t{0}, std::size_t{4}}) {
            Mesh* m = nullptr;
            for (std::size_t l = lod; l < 3 && !m; ++l)
                m = cell.groups[set + l];
            for (std::size_t l = lod; l-- > 0 && !m;)
                m = cell.groups[set + l];
            if (m)
                drawMesh(*m, identity, stats);
            if (Mesh* a = cell.groups[set + 3])
                drawMesh(*a, identity, stats);
        }
    }

    const Mat34 view = camera.transform;
    for (auto& fac : m_facades) {
        if (!frustum.intersectsSphere(fac.centre, fac.radius))
            continue;
        const float depth = viewDepth(view, fac.centre);
        if (depth - fac.radius > std::min(farClip, kBuildingMaxDist))
            continue;
        const auto lod = full ? std::optional(asset::Lod::High) : objectLod(depth, fac.radius, detail.objects);
        if (!lod)
            continue;
        ++stats.facadesDrawn;
        for (auto& g : fac.groups)
            if (Mesh* m = g[static_cast<std::size_t>(*lod)])
                drawMesh(*m, fac.world, stats);
    }
}

} // namespace mm2::game
