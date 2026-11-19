#include "game/Mm1CityRenderer.h"

#include "asset/Bms.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/CityRenderer.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace mm2::game {
namespace {

// Inferred from Open1560's StaticTerrainLodTable (cellrend.cpp; its use is
// not yet ported): a cell's high meshes out to 150 m, medium to 300 m, low
// beyond.
constexpr float kCellHigh = 150.0f;
constexpr float kCellMedium = 300.0f;
// BuildingMaxDist (cellrend.cpp): facades are not drawn beyond it.
constexpr float kBuildingMaxDist = 4000.0f;

// mmInstance::GetMeshSetSet's names for a group's levels.
constexpr std::array<std::string_view, 4> kLodSuffix{"h", "m", "l", "vl"};

// A facade side quad cut off below `clip` (its own frame): each corner
// under it moves up its column (the corners at the same x, z) to the clip
// height, its texture coordinates moving with it toward the column's top.
// False when nothing is left above the clip. Inferred: mmFacadeQuad's own
// clipping is unported; this reproduces what it hides (see Mm1Facade::sides).
bool clipMm1MeshBelow(asset::BmsMesh& m, float clip) {
    float top = -1e30f;
    for (const Vec3& p : m.positions)
        top = std::max(top, p.y);
    if (top <= clip + 0.01f)
        return false;
    const std::size_t n = m.adjunctCount();
    const bool uv = m.adjunctUv.size() == n;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 p = m.positions[m.adjunctPosition[i]];
        if (p.y >= clip || !uv)
            continue;
        // The highest corner of this corner's column.
        std::size_t best = i;
        for (std::size_t j = 0; j < n; ++j) {
            const Vec3 q = m.positions[m.adjunctPosition[j]];
            if (std::abs(q.x - p.x) < 0.01f && std::abs(q.z - p.z) < 0.01f &&
                q.y > m.positions[m.adjunctPosition[best]].y)
                best = j;
        }
        const float yTop = m.positions[m.adjunctPosition[best]].y;
        if (yTop > p.y) {
            const float t = (clip - p.y) / (yTop - p.y);
            m.adjunctUv[i] = m.adjunctUv[i] + (m.adjunctUv[best] - m.adjunctUv[i]) * t;
        }
    }
    for (Vec3& p : m.positions)
        p.y = std::max(p.y, clip);
    return true;
}

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
        if (std::getenv("OPENMM2_DEBUG_WATER_CELLS"))
            for (std::size_t g = 0; g < cell.groups.size(); ++g)
                if (cell.groups[g])
                    for (const auto& t : cell.groups[g]->textures)
                        if (str::iequals(t, "t_water"))
                            log::info("mm1 water cell {} group {}: y {:.2f}..{:.2f}, x {:.0f}..{:.0f}, z {:.0f}..{:.0f}", c.index, g,
                                      cell.groups[g]->model->bounds.min.y, cell.groups[g]->model->bounds.max.y,
                                      cell.groups[g]->model->bounds.min.x, cell.groups[g]->model->bounds.max.x,
                                      cell.groups[g]->model->bounds.min.z, cell.groups[g]->model->bounds.max.z);
        if (!box.valid())
            continue;
        cell.centre = (box.min + box.max) * 0.5f;
        cell.radius = (box.max - box.min).mag() * 0.5f;
        // Development aid: OPENMM2_DEBUG_FACADES_AT="x,z,radius" also lists the cells whose box covers the point.
        if (static const char* at = std::getenv("OPENMM2_DEBUG_FACADES_AT"); at) {
            float x = 0, z = 0, r = 0;
            if (std::sscanf(at, "%f,%f,%f", &x, &z, &r) == 3 && box.min.x - r <= x && x <= box.max.x + r && box.min.z - r <= z &&
                z <= box.max.z + r)
                log::info("mm1 cell at point: {} box ({:.0f},{:.1f},{:.0f})..({:.0f},{:.1f},{:.0f})", c.index, box.min.x, box.min.y,
                          box.min.z, box.max.x, box.max.y, box.max.z);
        }
        cell.radius = (box.max - box.min).mag() * 0.5f;
        m_cells.push_back(cell);
    }

    // The facades, buildings and shear pieces (mmCullCity::AddInstance,
    // InitFacade, mmBuildingInstance::Init, mmShearInstance::Init).
    using namespace city::Mm1InstFlag;
    struct FloatingRoof {
        std::string prefix;
        std::size_t index;
        float bottom;
    };
    std::vector<FloatingRoof> floatingRoofs;
    std::map<std::string, float> bodyTop;
    for (const auto& f : city.facades) {
        if (!(f.flags & Static))
            continue;
        Facade fac;
        const std::string name = str::lower(f.name);
        std::vector<std::string_view> groups;
        const bool shear = (f.flags & Shear) != 0;
        if (shear) {
            // InitMeshes: the model's own meshes, else FACADE, else BLDG (the
            // empty group name is the plain H/M/L/VL).
            fac.transform = city::mm1ShearMatrix(f.start, f.end, f.width);
            bool plain = false;
            for (std::size_t l = 0; l < kLodSuffix.size() && !plain; ++l)
                plain = m_vfs.exists(std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, name, kLodSuffix[l]));
            groups = {plain ? std::string_view{} : std::string_view{"facade"}};
        } else if (f.flags & Building) {
            fac.transform = city::mm1BuildingMatrix(f.start, f.end, f.sides);
            groups = {"facade", "grnd"};
        } else {
            fac.transform = city::mm1FacadeMatrix(f.start, f.end, f.width);
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
            // A side's neighbour: sides.x (LEFT) and sides.y (RIGHT) are
            // the height above `start` of the building beside it, -start.y
            // for none (inferred); the side is hidden below it.
            std::optional<float> clip;
            if (!shear && !(f.flags & Building) && (g == "left" || g == "right")) {
                const float above = g == "left" ? f.sides.x : f.sides.y;
                const float local = f.start.y + above - fac.transform.m3.y;
                if (local > 0.05f)
                    clip = local;
            }
            auto fill = [&](std::string_view group) {
                for (std::size_t l = 0; l < kLodSuffix.size(); ++l) {
                    if (lods[l])
                        continue;
                    lods[l] = group.empty()
                                  ? mesh(std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, name, kLodSuffix[l]))
                                  : mesh(std::format("{}bms/{}/{}_{}.bms", vfs::kMm1Prefix, name, group,
                                                     kLodSuffix[l]),
                                         clip);
                    any = any || lods[l];
                }
                if (!lods[0] && !any && !group.empty())
                    if ((lods[0] = mesh(std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, name, group), clip)))
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
            // lvlInstance::GetGeomSet does (MM1's own rule is unported; MM1
            // does draw tunnel02, which has only L and VL, up close).
            for (int l = 2; l >= 0; --l)
                if (!lods[static_cast<std::size_t>(l)])
                    lods[static_cast<std::size_t>(l)] = lods[static_cast<std::size_t>(l) + 1];
            // OpenMM2: a lower level of detail that is much smaller than the high one (under 80% of its
            // height or width, for a model 6 m or more across: Chicago's dfskys02, tfoff02, ibfact02,
            // tfdrug, ifdiner, tfoff04, ifwhse02 and ofhome01-03 have such levels; see mm2tool mm1audit)
            // would show a building shrinking or losing its walls at a distance: it takes the next more
            // detailed level instead.
            if (lods[0]) {
                const Aabb& hb = lods[0]->model->bounds;
                const float hh = hb.max.y - hb.min.y;
                const float hw = std::max(hb.max.x - hb.min.x, hb.max.z - hb.min.z);
                for (std::size_t l = 1; l < lods.size(); ++l)
                    if (lods[l] && lods[l] != lods[l - 1]) {
                        const Aabb& lb = lods[l]->model->bounds;
                        const float lh = lb.max.y - lb.min.y;
                        const float lw = std::max(lb.max.x - lb.min.x, lb.max.z - lb.min.z);
                        if ((hh >= 6.0f && lh < 0.8f * hh) || (hw >= 6.0f && lw < 0.8f * hw))
                            lods[l] = lods[l - 1];
                    }
            }
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
        // Development aid: OPENMM2_DEBUG_FACADES_AT="x,z,radius" lists the facades whose box covers that point.
        if (static const char* at = std::getenv("OPENMM2_DEBUG_FACADES_AT"); at) {
            float x = 0, z = 0, r = 0;
            if (std::sscanf(at, "%f,%f,%f", &x, &z, &r) == 3 && box.min.x - r <= x && x <= box.max.x + r && box.min.z - r <= z &&
                z <= box.max.z + r)
                log::info("mm1 facade at point: {} flags {:x} start ({:.1f},{:.1f},{:.1f}) end ({:.1f},{:.1f},{:.1f}) width {:.1f} sides ({:.1f},{:.1f}) box ({:.0f},{:.1f},{:.0f})..({:.0f},{:.1f},{:.0f})", name, f.flags, f.start.x, f.start.y, f.start.z, f.end.x, f.end.y, f.end.z, f.width, f.sides.x, f.sides.y, box.min.x,
                          box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
        }
        // OpenMM2: a roof piece of at most four triangles (the caps of towers whose walls the data lacks:
        // dt41_roof, dt07_roof) that floats more than 15 m above the tallest of its building's other
        // facades hangs in the air over the street; it is not drawn (inferred; MM1 may draw it).
        std::size_t triangles = 0;
        for (const auto& g : fac.groups)
            if (g[0])
                for (const auto& mesh : g[0]->model->meshes)
                    for (const auto& d : mesh.draws)
                        triangles += d.indexCount / 3;
        const std::string prefix = name.size() > 5 ? name.substr(0, name.size() - 5) : std::string();
        if (name.ends_with("_roof") && triangles <= 4) {
            floatingRoofs.push_back({prefix, m_facades.size(), box.min.y});
        } else {
            const auto cut = name.rfind('_');
            if (cut != std::string::npos)
                bodyTop[name.substr(0, cut)] = std::max(bodyTop[name.substr(0, cut)], box.max.y);
        }
        // Development aid: OPENMM2_DEBUG_HIDE_FACADES="name,name" hides the facades of those names (to find which
        // one draws a stray surface).
        if (static const char* hide = std::getenv("OPENMM2_DEBUG_HIDE_FACADES");
            hide && (std::string(hide) == "*" || (std::string(",") + hide + ",").find("," + name + ",") != std::string::npos))
            fac.hidden = true;
        m_facades.push_back(std::move(fac));
    }
    for (const auto& r : floatingRoofs)
        if (const auto it = bodyTop.find(r.prefix); it != bodyTop.end() && r.bottom > it->second + 15.0f) {
            m_facades[r.index].hidden = true;
            log::info("mm1: {}_roof floats {:.0f} m above its building and is not drawn", r.prefix, r.bottom - it->second);
        }

    // The drawbridges' leaves and gates (city/<city>.gizmo), at the gizmo
    // file's frames until game::world::Mm1Bridges moves them
    // (setBridgePart). They draw as facades do: plain H/M/L/VL meshes, plus
    // the leaves' RAIL group.
    for (const auto& set : city.bridges) {
        auto& indices = m_bridgeFacades.emplace_back();
        for (const auto& part : set.parts)
            indices.push_back(addPartFacade(part.model, part.matrix(), part.leaf()));
    }

    m_sky = mesh(std::string(vfs::kMm1Prefix) + "bms/mmsky.bms");

    // OpenMM2 water on MM1's water (mmCellRenderer::Init singles out T_WATER).
    // (Its look names too: the night, rain and snow alternates the sheet resolves T_WATER to; at night
    // "t_water_n" was not water, so the river kept its plain texture.)
    m_textures.setWaterTextures({std::string(kMm1TexturePrefix) + "t_water", std::string(kMm1TexturePrefix) + "t_water_n",
                                 std::string(kMm1TexturePrefix) + "t_water_fall", std::string(kMm1TexturePrefix) + "t_water_fall_n",
                                 std::string(kMm1TexturePrefix) + "t_water_win", std::string(kMm1TexturePrefix) + "t_water_win_n"});
    log::info("mm1: {} cells, {} facades, {} meshes ({} missing)", m_cells.size(), m_facades.size(), m_meshes.size(),
              m_missing);
}

int Mm1CityRenderer::addPartFacade(const std::string& model, const Mat34& transform, bool rail) {
    // Drawn as facades are: plain H/M/L/VL meshes (and a leaf's RAIL group).
    Facade fac;
    fac.transform = transform;
    fac.world = Mat44::fromMat34(fac.transform);
    Aabb box;
    for (std::string_view group : {std::string_view{}, std::string_view{"rail"}}) {
        if (!group.empty() && !rail)
            continue;
        std::array<Mesh*, 4> lods{};
        for (std::size_t l = 0; l < kLodSuffix.size(); ++l)
            lods[l] = mesh(group.empty()
                               ? std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, model, kLodSuffix[l])
                               : std::format("{}bms/{}/{}_{}.bms", vfs::kMm1Prefix, model, group, kLodSuffix[l]));
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
        return -1;
    fac.centre = (box.min + box.max) * 0.5f;
    // A moving part turns about its origin: the radius about the origin
    // covers every pose.
    fac.radius = std::max((box.max - box.min).mag() * 0.5f, (box.max - fac.transform.m3).mag());
    fac.radius = std::max(fac.radius, (box.min - fac.transform.m3).mag());
    fac.localCentre = fac.transform.inverse().transform(fac.centre);
    m_facades.push_back(std::move(fac));
    return static_cast<int>(m_facades.size()) - 1;
}

int Mm1CityRenderer::addMovingModel(const std::string& model) {
    return addPartFacade(str::lower(model), Mat34{}, false);
}

void Mm1CityRenderer::setMovingModel(int index, const Mat34& transform) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_facades.size())
        return;
    Facade& fac = m_facades[static_cast<std::size_t>(index)];
    fac.transform = transform;
    fac.world = Mat44::fromMat34(transform);
    fac.centre = transform.transform(fac.localCentre);
}

void Mm1CityRenderer::setBridgePart(std::size_t set, std::size_t part, const Mat34& transform, bool visible) {
    if (set >= m_bridgeFacades.size() || part >= m_bridgeFacades[set].size())
        return;
    const int index = m_bridgeFacades[set][part];
    setMovingModel(index, transform);
    if (index >= 0)
        m_facades[static_cast<std::size_t>(index)].hidden = !visible;
}

Mm1CityRenderer::Mesh* Mm1CityRenderer::mesh(const std::string& path, std::optional<float> clipBelow) {
    const std::string key = clipBelow ? std::format("{}#clip{:.3f}", path, *clipBelow) : path;
    if (auto it = m_meshes.find(key); it != m_meshes.end())
        return it->second.get();
    std::unique_ptr<Mesh> out;
    if (auto bytes = m_vfs.readAll(path)) {
        std::string err;
        auto bms = asset::parseBms(*bytes, &err);
        if (bms && clipBelow && !clipMm1MeshBelow(*bms, *clipBelow)) {
            m_meshes.emplace(key, nullptr); // all of it hidden
            return nullptr;
        }
        if (bms) {
            auto converted = asset::bmsToPkgMesh(*bms);
            asset::Pkg pkg;
            pkg.meshes.push_back(std::move(converted.mesh));
            pkg.paintjobs.push_back(converted.materials);
            if (const GpuModel* model = m_models.add("mm1:" + key, pkg); model && !model->meshes.empty()) {
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
    m_meshes.emplace(key, std::move(out));
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
                m_sheet ? m_sheet->resolve(raw, 0, night, m_textures.mm1Weather()) : std::string();
            if (resolved.empty())
                resolved = raw;
            m->materials[i].texture = std::string(kMm1TexturePrefix) + str::lower(resolved);
        }
    }
    if (std::getenv("OPENMM2_DEBUG_SNOWTEX")) {
        std::map<std::string, int> uses;
        for (auto& [path, m] : m_meshes)
            if (m)
                for (std::size_t i = 0; i < m->textures.size(); ++i)
                    ++uses[m->textures[i] + " -> " + m->materials[i].texture];
        for (const auto& [k, n] : uses)
            log::info("mm1 texture {} x{}", k, n);
    }
    m_night = night;
    m_weather = m_textures.mm1Weather();
}

void Mm1CityRenderer::drawSky(const Camera& camera, int skyIndex, bool snow, int& drawCalls) {
    if (!m_sky || skyIndex < 0)
        return;
    static constexpr std::array<char, 4> kTimes{'m', 'n', 's', 'd'};
    static constexpr std::array<char, 4> kWeathers{'c', 'f', 'f', 'r'}; // MM2's clear, cloudy, fog, rain (MM1's cloudy sky is f)
    const char weather = snow ? 's' : kWeathers[static_cast<std::size_t>(skyIndex % 4)];
    const std::string texture =
        std::format("{}sky_{}{}", kMm1TexturePrefix, kTimes[static_cast<std::size_t>(skyIndex / 4) % 4], weather);
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

void Mm1CityRenderer::drawMesh(Mesh& m, const Mat44& world, Stats& stats, bool cull) {
    MeshDrawOptions options;
    // Inferred until MM1's winding is confirmed in game: both faces.
    options.cull = cull;
    for (const auto& gm : m.model->meshes)
        stats.drawCalls += drawGpuMesh(m_device, m_textures, gm, m.materials, world, options);
}

void Mm1CityRenderer::draw(const Camera& camera, const Frustum& frustum, const DetailSettings& detail,
                           float farClip, Stats& stats) {
    if (!m_night || *m_night != m_textures.night() || m_weather != m_textures.mm1Weather())
        resolveMaterials(m_textures.night());
    const Vec3 eye = camera.position();
    const Mat44 identity = Mat44::fromMat34(Mat34::identity());
    const bool full = detail.fullDetail || detail.streetsFull;
    // OpenMM2 ray traced views' static capture in parts (DetailSettings::roomPart): each part draws the
    // cells and facades whose index is its number modulo the parts. (Without this every part drew the
    // whole city, and the ray scene held one copy of it per part.)
    const bool parts = detail.roomParts > 1;
    auto inPart = [&](std::size_t index) {
        return !parts || static_cast<int>(index % static_cast<std::size_t>(detail.roomParts)) == detail.roomPart;
    };

    std::size_t cellIndex = 0;
    for (auto& cell : m_cells) {
        if (!inPart(cellIndex++))
            continue;
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
            // The A group's polygons are drawn back-face culled: their hidden faces (a pit's top under a road,
            // e.g. cell 1062's A2 at the first bridge north of the start) lit black across the road. The other
            // groups still draw both faces (culling them all breaks surfaces; MM1's winding is not confirmed).
            if (Mesh* a = cell.groups[set + 3]; a)
                drawMesh(*a, identity, stats, true);
        }
    }

    const Mat34 view = camera.transform;
    std::size_t facadeIndex = 0;
    for (auto& fac : m_facades) {
        if (!inPart(facadeIndex++))
            continue;
        if (fac.hidden || !frustum.intersectsSphere(fac.centre, fac.radius))
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
