// The city for MM2's collision manager: cityLevel's room queries
// (FindRoomId, GetTouchedNeighbors), lvlSDL's polygon collection
// (sdlPage16::Collect, src/city/SdlCollect) and lvlLevel's instances
// (LoadInstances, lvlMultiRoomInstance, InitBoundTerrainLocal), from the code
// of midtown2.exe build 3393 (MM2Recomp). See docs/physics.md, "Collision".

#include "game/CityLevel.h"

#include "asset/Pkg.h"
#include "city/CityMesh.h"
#include "city/SdlCollect.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "data/CNumbers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mm2::game {
namespace {

// lvlLevel::LoadInstances: the .inst record's flags (the high half of its
// first word).
constexpr std::uint16_t kInstTerrainLocal = 0x100; // InitBoundTerrainLocal, room flag 0x20
constexpr std::uint16_t kInstBanger = 0x200;       // dgUnhitBangerInstance::RequestBanger
constexpr std::uint16_t kInstCollidable = 0x2000;  // collidable, placed by lvlMultiRoomInstance::Create
// A terrain-bound instance with this flag gets lvlInstance flags 0x110, not
// 0x130: wheel probes do not hit it.
constexpr std::uint16_t kInstNoWheels = 0x400;
// lvlSDL::CollidePolyToLevel's polygon buffer.
constexpr int kMaxLevelPolygons = 256;
// cityLevel's room flag of instance rooms (GetTouchedNeighbors takes them
// whatever the sphere).
constexpr std::uint8_t kRoomInstance = city::RoomFlag::Instance;

phys::TerrainData toTerrainData(const asset::TerrainBound& t) {
    phys::TerrainData d;
    d.grid.useHotEdges = t.useHotEdges;
    d.grid.size = t.size;
    d.grid.widthSections = static_cast<int>(t.widthSections);
    d.grid.heightSections = static_cast<int>(t.heightSections);
    d.grid.depthSections = static_cast<int>(t.depthSections);
    d.grid.sectionOffsets = t.sectionOffsets;
    d.grid.sectionCounts = t.sectionCounts;
    d.grid.sectionPolygons = t.sectionPolygons;
    d.grid.sectionSizeFactors = t.sectionSizeFactors;
    d.boxMin = t.min;
    d.boxMax = t.max;
    d.edges = t.edges;
    d.polygonEdges = t.polygonEdges;
    d.edgeNormals = t.edgeNormals;
    d.edgeCosines = t.edgeValues;
    return d;
}

float rowLength(const Vec3& v) {
    return std::sqrt(v.z * v.z + v.y * v.y + v.x * v.x);
}

// lvlInstance::GetRadius: the geometry set's radius. lvlFixedAny::Init takes
// the largest of its entries' radii (lvlInstance::GetGeomSet: the farthest
// vertex of any level of detail from the origin, modGetStatic) for the model
// itself and its "mask", "nonrandom", "refl" and "opaque" parts, not its
// shadow. Null without a model. (No retail collidable city model has any of
// those parts.)
bool radiusPart(std::string_view part) {
    return part.empty() || str::iequals(part, "mask") || str::iequals(part, "nonrandom") ||
           str::iequals(part, "refl") || str::iequals(part, "opaque");
}

float modelRadius(const vfs::Vfs& vfs, const std::string& name) {
    const auto bytes = vfs.readAll("geometry/" + str::lower(name) + ".pkg");
    if (!bytes)
        return 0.0f;
    const auto pkg = asset::parsePkg(*bytes);
    if (!pkg)
        return 0.0f;
    float radius = 0.0f;
    for (const auto& mesh : pkg->meshes)
        if (radiusPart(mesh.part))
            radius = std::max(radius, mesh.radius());
    return radius;
}

// The distance of the farther corner of a bound's box from its origin
// (InitBoundTerrainLocal raises the geometry set's radius to it), each
// corner's squared length summed (x*x + y*y) + z*z as the code does.
float boxCornerRadius(const phys::Bound& b) {
    float r2 = (b.boxMax.x * b.boxMax.x + b.boxMax.y * b.boxMax.y) + b.boxMax.z * b.boxMax.z;
    const float min2 = (b.boxMin.x * b.boxMin.x + b.boxMin.y * b.boxMin.y) + b.boxMin.z * b.boxMin.z;
    if (r2 < min2)
        r2 = min2;
    return std::sqrt(r2);
}

} // namespace

std::optional<asset::BoundGeometry> loadBoundFile(const vfs::Vfs& vfs, std::string_view name, bool binary) {
    const std::string path = "bound/" + str::lower(name) + (binary ? "_bound.bbnd" : "_bound.bnd");
    const auto bytes = vfs.readAll(path);
    if (!bytes)
        return std::nullopt;
    if (binary)
        return asset::parseBbnd(*bytes);
    return asset::parseBnd(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}

phys::GeometryData toGeometryData(const asset::BoundGeometry& bound,
                                  const std::function<const phys::Material*(const asset::BoundMaterial&)>& material) {
    phys::GeometryData g;
    g.vertices = bound.vertices;
    for (const auto& p : bound.polygons) {
        phys::GeometryData::Poly poly;
        poly.v = p.indices;
        poly.quad = p.quadToken || p.isQuad();
        poly.material = p.material;
        g.polys.push_back(poly);
    }
    for (const auto& m : bound.materials)
        g.materials.push_back(material ? material(m) : nullptr);
    return g;
}

const phys::Bound* StaticInstance::bound(int which) const {
    if (which != 0 && boxBound)
        return boxBound.get();
    return collisionBound.get();
}

CityLevel::CityLevel(const city::CityData& city, const vfs::Vfs& vfs,
                     const std::function<bool(std::string_view)>& isBanger)
    : m_city(city), m_locator(city.psdl, city.info.mapName) {
    // lvlMaterialMgr: city/materials.mtl, then the materials the bounds name.
    // Two views of the city's materials. lvlMaterialMgr (m_manager): its
    // built-in default material (lvlMaterial's constructor), then every new
    // name of city/materials.mtl in file order (cityLevel::Load; "_default" is
    // a new name), then the new names of the bound files (lvlMaterialMgr::Load:
    // "default" in a bound file is the built-in one). The level's polygons
    // and the objects' bounds use these. OpenMM2's MaterialTable (m_materials,
    // the default material first, see phys::MaterialTable) serves the
    // wheels' probe geometry.
    m_manager.push_back(phys::defaultBoundMaterial());
    auto managerIndex = [this](std::string_view name) -> int {
        for (std::size_t i = 0; i < m_manager.size(); ++i)
            if (str::iequals(m_manager[i].name, name))
                return static_cast<int>(i);
        return -1;
    };
    if (auto text = vfs.readAll("city/materials.mtl")) {
        std::string error;
        if (auto mats = phys::parseMaterials(std::string_view(reinterpret_cast<const char*>(text->data()), text->size()),
                                             &error)) {
            m_materials.add(*mats);
            for (const phys::Material& m : *mats)
                if (managerIndex(m.name) < 0)
                    m_manager.push_back(m);
        } else {
            log::warn("collision: city/materials.mtl: {}", error);
        }
    }
    if (city.mm1) {
        // OpenMM2 Enhanced: an MM1 city's collision is its cells' bounds.
        m_mm1 = std::make_unique<Mm1Level>(*city.mm1, vfs, managerIndex);
        if (city.mm1Ai && !std::getenv("OPENMM2_NO_EDGE_GUARD"))
            m_mm1->guardElevatedEdges(*city.mm1Ai);
        m_soup = m_mm1->buildSoup(m_materials);
        m_soup.finalize();
        return;
    }
    auto registerMaterials = [&](const asset::BoundGeometry& b) {
        for (const auto& m : b.materials) {
            if (str::iequals(m.name, "none"))
                continue;
            // lvlMaterialMgr::Load: a new name becomes an lvlMaterial with
            // the file's elasticity, friction, effect and sound ("none" is
            // 0) and the constructor's defaults for the rest (drag 0, width
            // 1, height 0, depth 0, no particles). A binary bound adds a
            // plain phMaterial instead (phMaterialMgr::Load); no retail
            // terrain bound introduces a new name, so both look alike here.
            phys::Material pm;
            pm.name = m.name;
            pm.elasticity = m.elasticity;
            pm.friction = m.friction;
            pm.effect = m.effect;
            pm.sound =
                str::istartsWith(m.sound, "none") ? 0 : static_cast<std::int16_t>(data::cAtoi(m.sound));
            pm.width = 1.0f;
            if (managerIndex(m.name) < 0)
                m_manager.push_back(pm);
            if (!str::iequals(m.name, "default") && m_materials.find(m.name) < 0)
                m_materials.add(pm);
        }
    };

    // lvlSDL's texture -> material table (lvlSDL::LoadBinary: materials.csv
    // names as 1-based manager indices, built before any bound file adds
    // materials).
    m_textureMaterials = city::sdlTextureMaterials(city.psdl, city.textureMaterials, [&](std::string_view name) {
        return city::sdlMaterialIndex(city.materials, name);
    });

    // The collidable instances (lvlLevel::LoadInstances): flag 0x2000 ones
    // with their geometry bound (lvlMultiRoomInstance::Create, which scales
    // the bound by the matrix's row lengths and normalises the matrix), flag
    // 0x100 ones with a terrain bound of their own space
    // (InitBoundTerrainLocal: .bbnd + .ter). Bangers (0x200) are BangerSet's;
    // every other instance is drawn only.
    struct Pending {
        const city::Instance* inst;
        std::optional<asset::BoundGeometry> geometry;
        std::optional<asset::TerrainBound> terrain;
        Vec3 scale{1, 1, 1};
    };
    std::vector<Pending> pending;
    for (const auto* list : {&city.instances, &city.aiInstances}) {
        for (const auto& inst : *list) {
            if ((inst.flags & kInstBanger) || (isBanger && isBanger(inst.name)))
                continue;
            Pending p;
            p.inst = &inst;
            if (inst.flags & kInstTerrainLocal) {
                // lvlInstance::InitBoundTerrainLocal: with a .ter, a terrain
                // bound (phBoundTerrain::Load: the .bbnd geometry, else the
                // .bnd); without one, a plain geometry bound from the .bnd.
                if (auto ter = vfs.readAll("bound/" + str::lower(inst.name) + "_bound.ter")) {
                    p.terrain = asset::parseTer(*ter);
                    p.geometry = loadBoundFile(vfs, inst.name, true);
                    if (!p.geometry)
                        p.geometry = loadBoundFile(vfs, inst.name, false);
                    // phBoundTerrain::Load accepts version 1.1 files whose
                    // polygon count is the geometry's; otherwise it fails
                    // and InitBoundTerrainLocal deletes the bound
                    // ("Malformed terrain").
                    if (!p.terrain || p.terrain->version != 1.1f ||
                        (p.geometry && p.terrain->polygonCount() != p.geometry->polygons.size())) {
                        ++m_missingBounds;
                        continue;
                    }
                } else {
                    p.geometry = loadBoundFile(vfs, inst.name, false);
                }
            } else if (inst.flags & kInstCollidable) {
                p.geometry = loadBoundFile(vfs, inst.name, false);
                const Mat34& m = inst.transform;
                const Vec3 s{rowLength(m.m0), rowLength(m.m1), rowLength(m.m2)};
                if (0.01f <= std::abs(s.x * s.x - 1.0f) || 0.01f <= std::abs(s.y * s.y - 1.0f) ||
                    0.01f <= std::abs(s.z * s.z - 1.0f))
                    p.scale = s;
            } else {
                continue;
            }
            if (!p.geometry) {
                ++m_missingBounds;
                log::debug("collision: {} (flags {:#x}) has no bound", inst.name, inst.flags);
                continue;
            }
            registerMaterials(*p.geometry);
            pending.push_back(std::move(p));
        }
    }

    // Now that the manager is complete its entries stay put: the bounds
    // point at them.
    auto materialOf = [&](const asset::BoundMaterial& m) -> const phys::Material* {
        const int index = managerIndex(m.name);
        return &m_manager[static_cast<std::size_t>(index < 0 ? 0 : index)];
    };
    m_roomInstances.resize(city.psdl.rooms.size());
    std::unordered_map<std::string, std::shared_ptr<const phys::Bound>> cache;
    std::unordered_map<std::string, float> modelRadii;
    for (Pending& p : pending) {
        const city::Instance& inst = *p.inst;
        auto si = std::make_unique<StaticInstance>();
        si->name = inst.name;
        si->m_matrix = inst.transform;
        phys::GeometryData data = toGeometryData(*p.geometry, materialOf);
        const bool scaled = p.scale.x != 1.0f || p.scale.y != 1.0f || p.scale.z != 1.0f;
        if (p.terrain) {
            const std::string key = "ter:" + str::lower(inst.name);
            if (auto it = cache.find(key); it != cache.end()) {
                si->collisionBound = it->second;
            } else {
                const phys::TerrainData terrain = toTerrainData(*p.terrain);
                std::shared_ptr<const phys::Bound> b = phys::makeTerrainBound(data, &terrain, true);
                cache[key] = b;
                si->collisionBound = b;
            }
        } else {
            if (scaled) {
                for (Vec3& v : data.vertices)
                    v = {v.x * p.scale.x, v.y * p.scale.y, v.z * p.scale.z};
                si->collisionBound = phys::makeGeometryBound(data);
                // Matrix34::Normalize.
                Mat34& m = si->m_matrix;
                m.m0 = m.m0 * (1.0f / p.scale.x);
                m.m1 = m.m1 * (1.0f / p.scale.y);
                m.m2 = m.m2 * (1.0f / p.scale.z);
            } else {
                const std::string key = "bnd:" + str::lower(inst.name);
                if (auto it = cache.find(key); it != cache.end()) {
                    si->collisionBound = it->second;
                } else {
                    std::shared_ptr<const phys::Bound> b = phys::makeGeometryBound(data);
                    cache[key] = b;
                    si->collisionBound = b;
                }
            }
        }
        if (!si->collisionBound) {
            ++m_missingBounds;
            log::debug("collision: {} (flags {:#x}): unusable bound ({} polys, terrain {})", inst.name, inst.flags,
                       p.geometry->polygons.size(), p.terrain ? p.terrain->polygonCount() : 0u);
            continue;
        }
        // lvlInstance::GetRadius (GetBoundSphere, TrivialCollideInstances):
        // the model's geometry set radius, which a terrain-local bound raises
        // to its box's farther corner (InitBoundTerrainLocal; the set is
        // shared by every instance of the model loaded after it).
        auto radius = modelRadii.find(inst.name);
        if (radius == modelRadii.end())
            radius = modelRadii.emplace(inst.name, modelRadius(vfs, inst.name)).first;
        if (inst.flags & kInstTerrainLocal)
            radius->second = std::max(radius->second, boxCornerRadius(*si->collisionBound));
        si->m_radius = radius->second;
        si->room = inst.room;
        // lvlLevel::LoadInstances: flags 0x130 (0x110 with record flag 0x400)
        // make the object collidable and terrain-collidable (which also keeps
        // it from being attached), and 0x20, the wheels' mask, is on all but
        // the terrain-bound ones with record flag 0x400.
        // lvlMultiRoomInstance's stand-ins answer IsCollidable false and
        // IsTerrainCollidable once per gather. A terrain-bound instance is an
        // lvlLandmark (record flag 0x100 without 0x200), whose IsCollidable
        // answers false too: movers gather it through their city flag only.
        si->collidable = false;
        si->terrainCollidable = true;
        si->wheelCollidable = !(inst.flags & kInstTerrainLocal) || !(inst.flags & kInstNoWheels);
        si->multiRoom = false;
        const Vec3 centre = si->position();
        if (inst.room > 0 && inst.room < m_roomInstances.size()) {
            if (inst.flags & kInstTerrainLocal) {
                // lvlLevel::MoveToRoom: its own room only (an instance room,
                // which GetTouchedNeighbors returns to every neighbour).
                m_roomInstances[inst.room].push_back(si.get());
            } else {
                // lvlMultiRoomInstance::Create: a stand-in in each room its
                // sphere reaches across the room's perimeter, not in the room
                // itself; the object goes to room 0. Reaching no room it is
                // never collided (nor drawn).
                int rooms[32];
                const int n = touchedNeighbors(rooms, 32, inst.room, centre, si->m_radius);
                if (n == 0)
                    log::debug("collision: {} in room {} reaches no neighbour", inst.name, inst.room);
                for (int k = 0; k < n; ++k)
                    if (rooms[k] > 0 && static_cast<std::size_t>(rooms[k]) < m_roomInstances.size())
                        m_roomInstances[static_cast<std::size_t>(rooms[k])].push_back(si.get());
            }
        }
        m_instances.push_back(std::move(si));
    }

    // Probe geometry: the PSDL mesh (facade bounds included) and the
    // collidable instances' bounds, for the wheels and line-of-sight tests
    // (street textures map to materials by city/materials.csv).
    std::unordered_map<std::string, std::string> byTexture;
    for (const auto& tm : city.textureMaterials)
        byTexture[str::lower(tm.texture)] = tm.material;
    city::CityMeshOptions opts;
    opts.includeFacadeBounds = true;
    const city::CityMesh mesh = city::buildCityMesh(city.psdl, opts);
    int streetPolygons = 0;
    for (const auto& room : mesh.rooms) {
        for (const auto& batch : room.batches) {
            if (batch.indices.empty())
                continue;
            phys::SoupGeometry g;
            std::string material = "default"; // lvlMaterialMgr entry 0
            if (const auto* name = city.psdl.texture(batch.texture); name && !name->empty())
                if (auto it = byTexture.find(str::lower(*name)); it != byTexture.end())
                    material = it->second;
            g.materialNames.push_back(material);
            g.vertices.reserve(batch.vertices.size());
            for (const auto& v : batch.vertices)
                g.vertices.push_back(v.position);
            for (std::size_t i = 0; i + 2 < batch.indices.size(); i += 3) {
                phys::SoupGeometry::Poly p;
                p.v = {batch.indices[i], batch.indices[i + 1], batch.indices[i + 2], 0};
                p.count = 3;
                g.polys.push_back(p);
            }
            streetPolygons += static_cast<int>(g.polys.size());
            m_soup.add(g, Mat34::identity(), m_materials);
        }
    }
    for (const auto& si : m_instances) {
        const auto* poly = dynamic_cast<const phys::BoundPolygonal*>(si->collisionBound.get());
        if (!poly)
            continue;
        phys::SoupGeometry g;
        g.vertices = poly->vertices;
        for (int i = 0; i < poly->numMaterials(); ++i)
            g.materialNames.push_back(poly->material(i).name);
        if (g.materialNames.empty())
            g.materialNames.push_back("default");
        for (const auto& pg : poly->polygons) {
            phys::SoupGeometry::Poly p;
            const int n = pg.vertexCount();
            for (int k = 0; k < n; ++k)
                p.v[static_cast<std::size_t>(k)] = pg.v[static_cast<std::size_t>(k)];
            p.count = static_cast<std::uint8_t>(n);
            p.material = pg.material < g.materialNames.size() ? pg.material : 0;
            g.polys.push_back(p);
        }
        m_soup.add(g, si->m_matrix, m_materials);
    }
    m_soup.finalize();
    log::info("collision: {} collidable instances ({} without a bound), {} probe polygons ({} street)",
              m_instances.size(), m_missingBounds, m_soup.size(), streetPolygons);
}

const phys::Material& CityLevel::material(int index) const {
    // lvlLevelBound::GetMaterial: 0 is the manager's default material, n its
    // entry n - 1.
    if (index <= 0 || static_cast<std::size_t>(index - 1) >= m_manager.size())
        return m_manager[0];
    return m_manager[static_cast<std::size_t>(index - 1)];
}

void CityLevel::removeSource(const InstanceSource* source) {
    std::erase(m_sources, source);
}

int CityLevel::findRoom(const Vec3& position, int hint) const {
    if (m_mm1)
        return m_mm1->findRoom(position, hint);
    // cityLevel::FindRoomId (RoomLocator): the hint room, its neighbours,
    // then the whole city (FullProbe); 0 off every room, where a body
    // collides with no level geometry.
    return m_locator.find(position, hint);
}

int CityLevel::neighbors(int* out, int max, int room) const {
    if (m_mm1)
        return m_mm1->neighbors(out, max, room);
    // cityLevel::GetNeighbors: the rooms across the room's perimeter edges,
    // each once, in perimeter order.
    if (room <= 0 || static_cast<std::size_t>(room) >= m_city.psdl.rooms.size())
        return 0;
    int count = 0;
    for (const auto& edge : m_city.psdl.rooms[static_cast<std::size_t>(room)].perimeter) {
        const int neighbor = edge.neighbor;
        if (neighbor == 0 || count >= max || std::find(out, out + count, neighbor) != out + count)
            continue;
        out[count++] = neighbor;
    }
    return count;
}

int CityLevel::touchedNeighbors(int* out, int max, int room, const Vec3& centre, float radius) const {
    if (m_mm1)
        return m_mm1->touchedNeighbors(out, max, room, centre, radius);
    return cityTouchedNeighbors(m_city.psdl, out, max, room, centre, radius);
}

int cityTouchedNeighbors(const city::Psdl& psdl, int* out, int max, int room, const Vec3& centre,
                         float radius) {
    // cityLevel::GetTouchedNeighbors: the rooms across the room's perimeter
    // edges the sphere reaches (in the ground plane), each once; instance
    // rooms whatever the sphere.
    if (room <= 0 || static_cast<std::size_t>(room) >= psdl.rooms.size() || max <= 0)
        return 0;
    const city::PsdlRoom& r = psdl.rooms[static_cast<std::size_t>(room)];
    const auto& verts = psdl.vertices;
    const float r2 = radius * radius;
    const auto n = r.perimeter.size();
    int count = 0;
    auto seen = [&](int id) {
        for (int k = 0; k < count; ++k)
            if (out[k] == id)
                return true;
        return false;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const int neighbor = r.perimeter[i].neighbor;
        if (neighbor == 0 || seen(neighbor) || static_cast<std::size_t>(neighbor) >= psdl.rooms.size())
            continue;
        bool touched = false;
        if (psdl.rooms[static_cast<std::size_t>(neighbor)].flags & kRoomInstance) {
            touched = true;
        } else {
            const std::size_t next = i + 1 != n ? i + 1 : 0;
            const auto ia = r.perimeter[i].vertex;
            const auto ib = r.perimeter[next].vertex;
            if (ia >= verts.size() || ib >= verts.size())
                continue;
            const Vec3& p1 = verts[ia];
            if (ia == ib) {
                const float dx = centre.x - p1.x, dz = centre.z - p1.z;
                touched = dx * dx + dz * dz < r2;
            } else {
                const Vec3& p2 = verts[ib];
                const float ex = p2.x - p1.x, ez = p2.z - p1.z;
                float reach = ex * ex + ez * ez + r2;
                reach = reach + reach;
                const float ax = p1.x - centre.x, az = p1.z - centre.z;
                if (ax * ax + az * az < reach) {
                    const float bx = p2.x - centre.x, bz = p2.z - centre.z;
                    if (bx * bx + bz * bz < reach) {
                        const float cross = ax * bz - az * bx;
                        const float dx = bx - ax, dz = bz - az;
                        touched = 0.0f < (dx * dx + dz * dz) * r2 - cross * cross;
                    }
                }
            }
        }
        if (!touched)
            continue;
        out[count++] = neighbor;
        if (count == max)
            break;
    }
    return count;
}

void CityLevel::collect(const int* rooms, int count, const Vec3& centre, float radius,
                        phys::LevelBound& out) const {
    if (m_mm1) {
        m_mm1->collect(rooms, count, centre, radius, out);
        return;
    }
    // lvlSDL::CollidePolyToLevel's collection: sdlPage16::Collect of each
    // room into one buffer of at most 256 polygons.
    thread_local city::SdlPolyBuffer buffer;
    buffer.reset(m_city.psdl);
    const city::SdlSphere sphere{centre, radius};
    int total = 0;
    // One resume state for the whole call (an overflow makes the next room
    // start at the attribute where it happened, as in MM2).
    std::uint32_t state = 0;
    for (int k = 0; k < count; ++k) {
        const int room = rooms[k];
        if (room <= 0 || static_cast<std::size_t>(room) >= m_city.psdl.rooms.size())
            continue;
        total += city::collectRoomPolygons(m_city.psdl, static_cast<std::size_t>(room), &sphere, m_textureMaterials,
                                           buffer, kMaxLevelPolygons - total, nullptr, &state);
        if (state != 0)
            log::debug("collision: CollidePolyToLevel: buffer overflow in room {}", room);
    }
    out.clear();
    for (const city::SdlPoly& p : buffer.polys) {
        const int n = p.v[3] != 0 ? 4 : 3;
        Vec3 corners[4];
        for (int i = 0; i < n; ++i)
            corners[i] = buffer.vertices[p.v[static_cast<std::size_t>(i)]];
        out.addPolygon(corners, n, p.normal, p.material);
    }
}

int CityLevel::roomFlags(int room) const {
    if (room <= 0 || static_cast<std::size_t>(room) >= m_city.psdl.rooms.size())
        return 0;
    return m_city.psdl.rooms[static_cast<std::size_t>(room)].flags;
}

int CityLevel::roomInfoFlags(int room) const {
    if (room <= 0 || static_cast<std::size_t>(room) >= m_city.levelRoomFlags.size())
        return 0;
    return m_city.levelRoomFlags[static_cast<std::size_t>(room)];
}

void CityLevel::collectProbe(int room, const Vec3& centre, float radius, phys::LevelBound& out) const {
    if (m_mm1) {
        collect(&room, 1, centre, radius, out);
        return;
    }
    // sdlPage16::CollideSegment's collection for lvlSDL::CollideProbe:
    // Collect with the room marked as the probed one, in batches of 256
    // polygons, each resuming at the attribute where the last overflowed,
    // until a batch finishes the room, comes back empty or stops where the
    // last one did (MM2 reports "Primitive too large").
    struct ProbeBuffer {
        const city::Psdl* psdl = nullptr;
        city::SdlPolyBuffer buffer;
    };
    thread_local ProbeBuffer probe;
    const city::Psdl& psdl = m_city.psdl;
    if (probe.psdl != &psdl || probe.buffer.psdlVertexCount != psdl.vertices.size() ||
        probe.buffer.vertices.size() < psdl.vertices.size()) {
        probe.buffer.reset(psdl);
        probe.psdl = &psdl;
    } else {
        // SdlPolyBuffer::reset without copying the PSDL's vertices again
        // (Collect only appends to them).
        probe.buffer.vertices.resize(probe.buffer.psdlVertexCount);
        probe.buffer.polys.clear();
        probe.buffer.vertexBudget = city::kSdlGeneratedVertexBudget;
    }
    out.clear();
    if (room <= 0 || static_cast<std::size_t>(room) >= psdl.rooms.size())
        return;
    const city::SdlSphere sphere{centre, radius};
    std::uint32_t state = 0;
    std::uint32_t previous = 0;
    for (;;) {
        const int added =
            city::collectRoomPolygons(psdl, static_cast<std::size_t>(room), &sphere, m_textureMaterials,
                                      probe.buffer, kMaxLevelPolygons, nullptr, &state,
                                      static_cast<std::uint16_t>(room));
        if (added == 0 || state == 0 || state == previous)
            break;
        previous = state;
    }
    for (const city::SdlPoly& p : probe.buffer.polys) {
        const int n = p.v[3] != 0 ? 4 : 3;
        Vec3 corners[4];
        for (int i = 0; i < n; ++i)
            corners[i] = probe.buffer.vertices[p.v[static_cast<std::size_t>(i)]];
        out.addPolygon(corners, n, p.normal, p.material);
    }
}

void CityLevel::instances(int room, std::vector<phys::Instance*>& out) const {
    // lvlLevel::MoveToRoom links a movable instance at the head of the
    // room's list and a static one (flag 0x400) in front of the room's
    // earlier statics, after every movable one: the sources' instances
    // first, then the statics newest first.
    for (const InstanceSource* s : m_sources)
        s->instancesIn(room, out);
    if (room > 0 && static_cast<std::size_t>(room) < m_roomInstances.size()) {
        const auto& list = m_roomInstances[static_cast<std::size_t>(room)];
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            out.push_back(*it);
    }
}

} // namespace mm2::game
