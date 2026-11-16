#include "game/Mm1Level.h"

#include "asset/Bms.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <format>

namespace mm2::game {
namespace {

// lvlSDL::CollidePolyToLevel's buffer size, kept for MM1's polygons too.
constexpr int kMaxPolygons = 256;

// Chicago's cells with bounds of their own (renderweb.cpp LoadRoomBounds).
std::string_view specialBound(std::string_view city, int cell) {
    if (city != "chicago")
        return {};
    switch (cell) {
    case 60: return "dl60_bnd";           // CHICAGO_CELL_CONSTRUCTION
    case 24: case 174: return "dl24_bnd"; // CHICAGO_CELL_WRIGLEY_OUTER / _INNER
    default: return {};
    }
}

float segmentDistanceXZ(const Vec3& p, const Vec3& a, const Vec3& b) {
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float len2 = dx * dx + dz * dz;
    float t = len2 > 0 ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = a.x + t * dx - p.x, ez = a.z + t * dz - p.z;
    return std::sqrt(ex * ex + ez * ez);
}

} // namespace

std::string_view Mm1Level::mm2MaterialFor(std::string_view mm1Name) {
    const std::string n = str::lower(mm1Name);
    if (n.find("water") != std::string::npos)
        return "deepwater"; // MM1's lake and river: the car is lost (inferred to match)
    if (n.find("grass") != std::string::npos || n == "bush" || n == "shrub")
        return "grass";
    if (n == "dirt")
        return "dirt";
    if (n == "flagstone" || n.find("cob") != std::string::npos)
        return "cobblestone";
    return "_default";
}

Mm1Level::Mm1Level(const city::Mm1City& city, const vfs::Vfs& vfs,
                   const std::function<int(std::string_view)>& managerIndex) {
    const std::string prefix(vfs::kMm1Prefix);
    // MM1 physics material (1-based) -> the manager's index + 1, and a soup name.
    std::vector<std::uint8_t> toManager(1, 0), toSoup(1, 0);
    m_soupNames.push_back("_default");
    if (auto b = vfs.readAll(prefix + "mtl/physics.db")) {
        std::string err;
        if (auto mats = asset::parseMm1PhysicsDb(*b, &err)) {
            for (const auto& m : *mats) {
                const std::string_view mm2 = mm2MaterialFor(m.name);
                const int idx = managerIndex(mm2);
                toManager.push_back(static_cast<std::uint8_t>(idx >= 0 ? idx + 1 : 0));
                auto it = std::ranges::find(m_soupNames, mm2);
                if (it == m_soupNames.end()) {
                    m_soupNames.emplace_back(mm2);
                    it = m_soupNames.end() - 1;
                }
                toSoup.push_back(static_cast<std::uint8_t>(it - m_soupNames.begin()));
            }
        } else {
            log::warn("mm1: mtl/physics.db: {}", err);
        }
    }

    // Each cell's bound (LoadRoomBounds).
    m_rooms.resize(static_cast<std::size_t>(std::max(city.maxCells, 1)));
    int missing = 0;
    for (const auto& cell : city.cells) {
        std::string dir(specialBound(city.name, cell.index));
        std::optional<std::vector<std::byte>> bytes;
        if (!dir.empty())
            bytes = vfs.readAll(std::format("{}bnd/{}/bound{:02}.bnd", prefix, dir, cell.index));
        if (!bytes)
            bytes = vfs.readAll(std::format("{}bnd/{}/bound{:02}.bnd", prefix, city.cellMeshDir(cell.index),
                                            cell.index));
        if (!bytes) {
            ++missing;
            continue;
        }
        std::string err;
        auto bound = asset::parseMm1Bound(*bytes, &err);
        if (!bound) {
            log::warn("mm1: bound of cell {}: {}", cell.index, err);
            continue;
        }
        auto& room = m_rooms[static_cast<std::size_t>(cell.index)];
        for (const auto& p : bound->polygons) {
            Poly poly;
            poly.count = p.quad() ? 4 : 3;
            for (int i = 0; i < poly.count; ++i)
                poly.v[static_cast<std::size_t>(i)] = bound->vertices[static_cast<std::size_t>(p.vertices[static_cast<std::size_t>(i)])];
            poly.normal = p.normal;
            // OpenMM2: a polygon steeper than 60 degrees but not quite vertical (the bevelled sides
            // of Chicago's 1 m ramp-split barriers have normal.y 0.21) is a wall: its slight upward
            // push would lift a wheel that touches it, riding the car up onto the barrier.
            if (poly.normal.y > 0.05f && poly.normal.y < 0.5f) {
                poly.normal.y = 0.0f;
                poly.normal = poly.normal * (1.0f / std::sqrt(poly.normal.x * poly.normal.x + poly.normal.z * poly.normal.z));
            }
            // MM2's collector expects the corners counter-clockwise about
            // the normal; MM1's mostly are, the rest are turned round.
            const Vec3 cross = (poly.v[1] - poly.v[0]).cross(poly.v[2] - poly.v[0]);
            if (cross.dot(poly.normal) < 0.0f)
                std::reverse(poly.v.begin(), poly.v.begin() + poly.count);
            // OpenMM2: MM1's bounds hold zero-area polygons (three equal corners, normal straight up, e.g.
            // at Chicago's off ramp (-196.2, 8.7, 1220.1)); a wheel probe takes one for a ground plane
            // and rides up onto it (the left side of the car lifts, rolling it over). Dropped.
            {
                const Vec3 c1 = (poly.v[1] - poly.v[0]).cross(poly.v[2] - poly.v[0]);
                float area2 = c1.dot(c1);
                if (poly.count == 4) {
                    const Vec3 c2 = (poly.v[2] - poly.v[0]).cross(poly.v[3] - poly.v[0]);
                    area2 = std::max(area2, c2.dot(c2));
                }
                if (area2 < 1e-8f) {
                    ++m_degenerate;
                    continue;
                }
            }
            poly.room = cell.index;
            poly.material = p.material < toManager.size() ? toManager[p.material] : 0;
            poly.soupMaterial = p.material < toSoup.size() ? toSoup[p.material] : 0;
            for (int i = 0; i < poly.count; ++i)
                poly.box.expand(poly.v[static_cast<std::size_t>(i)]);
            room.polys.push_back(poly);
            ++m_polygonCount;
        }
    }

    // The portals: neighbours and the edges between them (the polygon's
    // bottom edge, from the last corner to the third).
    for (const auto& portal : city.portals) {
        const std::array<Vec3, 2> edge{portal.polygon.back(), portal.polygon[2]};
        for (auto [a, b] : {std::pair{portal.cell1, portal.cell2}, std::pair{portal.cell2, portal.cell1}})
            if (a > 0 && static_cast<std::size_t>(a) < m_rooms.size())
                m_rooms[static_cast<std::size_t>(a)].portals.push_back({b, edge});
    }

    // The footprints (LoadHitId): the city's cell locator.
    m_locator = city.cellLocator ? &*city.cellLocator : nullptr;

    // OpenMM2: holes in MM1's ground. A few bounds leave out a polygon under
    // ground their cell meshes draw (cell 999's sidewalk at x -77..-67, z
    // 511..516; the lawn beside cell 40 at -60, 470), and a car falls through
    // there. Every upward, non-water triangle of a cell's meshes with no
    // ground under its middle, but ground at its own height under one of its
    // corners (a hole in a surface, not a roof), becomes ground of that
    // neighbour's material. Inferred: whether MM1's own collision covers
    // these spots is unknown.
    buildGrid();
    int filled = 0;
    {
        std::vector<std::pair<int, Poly>> fill;
        for (const auto& cell : city.cells) {
            if (cell.index <= 0 || static_cast<std::size_t>(cell.index) >= m_rooms.size())
                continue;
            for (std::string_view g : {"H", "A", "H2", "A2"}) {
                auto bytes = vfs.readAll(city.cellMeshPath(cell.index, g));
                if (!bytes && g == "H")
                    bytes = vfs.readAll(std::format("{}bms/{}/cull{:02}.bms", prefix, city.cellMeshDir(cell.index),
                                                    cell.index));
                if (!bytes)
                    continue;
                auto mesh = asset::parseBms(*bytes);
                if (!mesh)
                    continue;
                for (const auto& sf : mesh->surfaces) {
                    if (sf.texture > 0 && sf.texture <= mesh->textures.size() &&
                        str::lower(mesh->textures[sf.texture - 1u].name).find("water") != std::string::npos)
                        continue;
                    const int k = sf.quad() ? 4 : 3;
                    for (int t = 0; t < k - 2; ++t) {
                        auto at = [&](int i) {
                            return mesh->positions[mesh->adjunctPosition[sf.corners[static_cast<std::size_t>(i)]]];
                        };
                        const std::array<Vec3, 3> v{at(0), at(t + 1), at(t + 2)};
                        Vec3 n = (v[1] - v[0]).cross(v[2] - v[0]);
                        const float len = n.mag();
                        if (len < 1e-4f || n.y / len < 0.7f) {
                            if (len >= 1e-4f && -n.y / len >= 0.7f)
                                n = n * -1.0f; // wound the other way
                            else
                                continue;
                        }
                        n = n * (1.0f / len);
                        const Vec3 mid = (v[0] + v[1] + v[2]) * (1.0f / 3.0f);
                        if (groundNear(mid, kFillTolerance, kFillTolerance))
                            continue;
                        const Poly* next = nullptr;
                        for (const Vec3& c : v)
                            if (!next)
                                next = groundPolyNear(c + (mid - c) * 0.05f, kFillTolerance, kFillTolerance);
                        if (!next) {
                            for (const Vec3& c : v)
                                if (!next)
                                    next = groundPolyNear(c, kFillTolerance, kFillTolerance);
                        }
                        if (!next)
                            continue;
                        Poly poly;
                        poly.count = 3;
                        poly.v = {v[0], v[1], v[2], Vec3{}};
                        poly.normal = n;
                        const Vec3 cross = (poly.v[1] - poly.v[0]).cross(poly.v[2] - poly.v[0]);
                        if (cross.dot(poly.normal) < 0.0f)
                            std::swap(poly.v[1], poly.v[2]);
                        poly.material = next->material;
                        poly.soupMaterial = next->soupMaterial;
                        for (int i = 0; i < 3; ++i)
                            poly.box.expand(poly.v[static_cast<std::size_t>(i)]);
                        fill.emplace_back(cell.index, poly);
                    }
                }
            }
        }
        for (auto& [room, poly] : fill) {
            poly.room = room;
            m_rooms[static_cast<std::size_t>(room)].polys.push_back(poly);
            ++m_polygonCount;
            ++filled;
        }
    }

    // The building walls. MM1's bounds hold the ground, kerbs and barriers
    // but no building fronts: the drivable area is the footprints' union, and
    // leaving it is hitting a building. Inferred, until MM1's own test
    // (mmBoundTemplate's 2D edges) is ported: every footprint edge with no
    // footprint just beyond it, at about its height, becomes a wall from
    // the edge up, collected with the footprint's cell.
    int walls = 0, opened = 0;
    // OpenMM2: no wall where MM1's bounds have ground just beyond the edge
    // from just below its height to 6 m above it (a road the footprints
    // miss, or one over the river's footprints, which lie 5 m down, such as
    // a drawbridge approach: buildings stand on no ground polygons), nor where a
    // drawbridge leaf's deck lies beyond it. Inferred.
    buildGrid();
    // OpenMM2: a long wall polygon (40 m or more along, 1.5-10 m tall) is raised by 3 m at the top.
    // Chicago's ramp walls (the right side of the off ramp, 85 m) hang from the road's edge DOWN (the
    // top is at road level), so a car's body passes over them; MM1 has a wall there that holds the
    // car in. Short ones of the same kind (the edge of the parking space at the ramp's foot, 28 m)
    // stay as they are: there the car drives in. Inferred from those two cases.
    {
        int raised = 0;
        for (auto& room : m_rooms)
            for (Poly& poly : room.polys) {
                if (std::fabs(poly.normal.y) >= 0.7f)
                    continue;
                // The wall's real thickness: the vertical edges (corners with the same x and z), not the
                // spread of heights along a sloping wall, which would take a long kerb on a slope for a wall.
                float thick = 0.0f, longest = 0.0f;
                for (int i = 0; i < poly.count; ++i) {
                    const Vec3& vi = poly.v[static_cast<std::size_t>(i)];
                    for (int j = i + 1; j < poly.count; ++j) {
                        const Vec3& vj = poly.v[static_cast<std::size_t>(j)];
                        const float h = std::hypot(vi.x - vj.x, vi.z - vj.z);
                        longest = std::max(longest, h);
                        if (h < 0.05f)
                            thick = std::max(thick, std::fabs(vi.y - vj.y));
                    }
                }
                poly.thickness = thick;
                if (std::fabs(poly.normal.y) >= 0.3f || longest < 40.0f || thick < 1.5f || thick > 10.0f)
                    continue;
                for (int i = 0; i < poly.count; ++i) {
                    Vec3& vi = poly.v[static_cast<std::size_t>(i)];
                    // The top corner of each vertical edge.
                    bool top = false;
                    for (int j = 0; j < poly.count; ++j)
                        if (j != i) {
                            const Vec3& vj = poly.v[static_cast<std::size_t>(j)];
                            if (std::hypot(vi.x - vj.x, vi.z - vj.z) < 0.05f && vi.y > vj.y)
                                top = true;
                        }
                    if (top)
                        vi.y += 3.0f;
                }
                poly.box = Aabb{};
                for (int i = 0; i < poly.count; ++i)
                    poly.box.expand(poly.v[static_cast<std::size_t>(i)]);
                ++raised;
            }
        log::info("mm1 collision: {} long walls raised", raised);
    }
    std::vector<std::pair<Mat34, Aabb>> decks;
    for (const auto& set : city.bridges)
        for (const auto& part : set.parts)
            if (part.leaf())
                decks.push_back({part.matrix(), Aabb{{-20.5f, -5.0f, -33.0f}, {20.5f, 5.0f, 0.5f}}});
    auto drivableBeyond = [&](const Vec3& q) {
        for (const auto& [m, box] : decks) {
            const Vec3 d = q - m.m3;
            const Vec3 local{d.dot(m.m0), d.dot(m.m1), d.dot(m.m2)};
            if (local.x >= box.min.x && local.x <= box.max.x && local.y >= box.min.y && local.y <= box.max.y &&
                local.z >= box.min.z && local.z <= box.max.z)
                return true;
        }
        return groundNear(q, kWallGroundBelow, kWallGroundAbove);
    };
    static const std::vector<city::Mm1CellLocator::Footprint> kNone;
    for (const auto& f : m_locator ? m_locator->footprints() : kNone) {
        if (f.cell <= 0 || static_cast<std::size_t>(f.cell) >= m_rooms.size())
            continue;
        Vec3 centre{};
        for (int i = 0; i < f.count; ++i)
            centre = centre + f.v[static_cast<std::size_t>(i)];
        centre = centre * (1.0f / static_cast<float>(f.count));
        for (int i = 0; i < f.count; ++i) {
            const Vec3 a = f.v[static_cast<std::size_t>(i)];
            const Vec3 b = f.v[static_cast<std::size_t>((i + 1) % f.count)];
            const float dx = b.x - a.x, dz = b.z - a.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 0.05f)
                continue;
            // The outward side: away from the footprint's centre.
            Vec3 out{dz / len, 0.0f, -dx / len};
            const Vec3 mid = (a + b) * 0.5f;
            if ((mid - centre).dot(out) < 0.0f)
                out = out * -1.0f;
            // OpenMM2: the edge in pieces of at most kWallPiece, each tested
            // on its own, so a long edge with ground beyond only part of it
            // (the road along Soldier Field: the stadium beyond its middle,
            // lawns beyond its ends) is walled only where nothing lies beyond.
            auto addWall = [&](const Vec3& wa, const Vec3& wb) {
                Poly wall;
                wall.count = 4;
                // Counter-clockwise seen from outside the drivable area... the
                // normal faces back into it.
                wall.normal = out * -1.0f;
                wall.thickness = kWallHeight + kWallBelow;
                wall.v = {wa + Vec3{0, -kWallBelow, 0}, wa + Vec3{0, kWallHeight, 0}, wb + Vec3{0, kWallHeight, 0},
                          wb + Vec3{0, -kWallBelow, 0}};
                const Vec3 cross = (wall.v[1] - wall.v[0]).cross(wall.v[2] - wall.v[0]);
                if (cross.dot(wall.normal) < 0.0f)
                    std::reverse(wall.v.begin(), wall.v.end());
                for (const auto& v : wall.v)
                    wall.box.expand(v);
                m_rooms[static_cast<std::size_t>(f.cell)].polys.push_back(wall);
                m_walls.emplace_back(wa, wb);
                ++walls;
                ++m_polygonCount;
            };
            const int pieces = std::max(1, static_cast<int>(std::ceil(len / kWallPiece)));
            auto at = [&](int k) { return a + (b - a) * (static_cast<float>(k) / static_cast<float>(pieces)); };
            int runStart = -1; // first piece of the current run of walled pieces
            for (int k = 0; k <= pieces; ++k) {
                bool blocked = false;
                if (k < pieces) {
                    const Vec3 pm = (at(k) + at(k + 1)) * 0.5f;
                    const Vec3 probe = pm + out * kWallProbe;
                    const bool fpNear = m_locator->footprintNear(probe, pm.y);
                    const bool beyond = !fpNear && drivableBeyond(probe);
                    if (fpNear) {
                    } else if (beyond) {
                        ++opened;
                    } else {
                        blocked = true;
                    }
                    if (static const char* dbg = std::getenv("OPENMM2_DEBUG_WALLS_NEAR"); dbg) {
                        float nx = 0, nz = 0, nr = 8;
                        if (std::sscanf(dbg, "%f,%f,%f", &nx, &nz, &nr) >= 2 && std::hypot(pm.x - nx, pm.z - nz) < nr)
                            log::info("wall edge piece ({:.1f},{:.1f},{:.1f}) cell {} out ({:.2f},{:.2f}): footprint beyond {} ground beyond {} -> {}",
                                      pm.x, pm.y, pm.z, f.cell, out.x, out.z, fpNear, beyond, blocked ? "WALL" : "open");
                    }
                }
                if (blocked && runStart < 0)
                    runStart = k;
                if (!blocked && runStart >= 0) {
                    addWall(at(runStart), at(k));
                    runStart = -1;
                }
            }
        }
    }
    // The drawbridges' leaves and gates move (game::world::Mm1Bridges, an
    // instance source of the level), so none of their polygons are here.
    buildGrid();
    log::info("mm1 collision: {} polygons in {} cells ({} zero-area dropped, {} without a bound; {} building walls, {} edges left open over ground, {} ground holes filled), {} footprints",
              m_polygonCount, city.cells.size(), m_degenerate, missing, walls, opened, filled, m_locator ? m_locator->footprints().size() : 0);
}

int Mm1Level::findRoom(const Vec3& position, int hint) const {
    const int room = m_locator ? m_locator->find(position, hint) : 0;
    if (room > 0)
        return room;
    // OpenMM2: no footprint here (a hole the ground filling covered, as by
    // cell 40's lawn): the cell of the ground under the position, so
    // whatever stands on it stays in the world.
    if (const Poly* ground = groundPolyNear(position, 3.0f, 1.0f))
        return ground->room;
    return 0;
}

int Mm1Level::neighbors(int* out, int max, int room) const {
    if (room <= 0 || static_cast<std::size_t>(room) >= m_rooms.size())
        return 0;
    int count = 0;
    for (const auto& [other, edge] : m_rooms[static_cast<std::size_t>(room)].portals) {
        if (count >= max || std::find(out, out + count, other) != out + count)
            continue;
        out[count++] = other;
    }
    return count;
}

int Mm1Level::touchedNeighbors(int* out, int max, int room, const Vec3& centre, float radius) const {
    if (room <= 0 || static_cast<std::size_t>(room) >= m_rooms.size())
        return 0;
    int count = 0;
    for (const auto& [other, edge] : m_rooms[static_cast<std::size_t>(room)].portals) {
        if (count >= max || std::find(out, out + count, other) != out + count)
            continue;
        if (segmentDistanceXZ(centre, edge[0], edge[1]) <= radius)
            out[count++] = other;
    }
    return count;
}

bool Mm1Level::groundNear(const Vec3& p, float below, float above) const {
    return groundPolyNear(p, below, above) != nullptr;
}

const Mm1Level::Poly* Mm1Level::groundPolyNear(const Vec3& p, float below, float above) const {
    if (m_grid.empty())
        return nullptr;
    const int x = static_cast<int>((p.x - m_gridMinX) / kGridCell);
    const int z = static_cast<int>((p.z - m_gridMinZ) / kGridCell);
    if (x < 0 || z < 0 || x >= m_gridNx || z >= m_gridNz)
        return nullptr;
    for (const PolyRef& ref : m_grid[static_cast<std::size_t>(z) * static_cast<std::size_t>(m_gridNx) + static_cast<std::size_t>(x)]) {
        const Poly& poly = m_rooms[ref.room].polys[ref.index];
        if (poly.normal.y < 0.7f || p.x < poly.box.min.x || p.x > poly.box.max.x || p.z < poly.box.min.z ||
            p.z > poly.box.max.z)
            continue;
        // Inside the polygon seen from above (the corners turn one way).
        bool inside = true;
        float sign = 0.0f;
        for (int i = 0; i < poly.count && inside; ++i) {
            const Vec3& a = poly.v[static_cast<std::size_t>(i)];
            const Vec3& b = poly.v[static_cast<std::size_t>((i + 1) % poly.count)];
            const float c = (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
            if (c * sign < 0.0f)
                inside = false;
            else if (c != 0.0f)
                sign = c;
        }
        if (!inside)
            continue;
        // The plane's height there: normal . v + d == 0.
        const float y = poly.v[0].y - (poly.normal.x * (p.x - poly.v[0].x) + poly.normal.z * (p.z - poly.v[0].z)) / poly.normal.y;
        if (y >= p.y - below && y <= p.y + above)
            return &poly;
    }
    return nullptr;
}

void Mm1Level::buildGrid() {
    Aabb all;
    for (const auto& room : m_rooms)
        for (const Poly& p : room.polys) {
            all.expand(p.box.min);
            all.expand(p.box.max);
        }
    if (!all.valid())
        return;
    m_gridMinX = all.min.x;
    m_gridMinZ = all.min.z;
    m_gridNx = static_cast<int>((all.max.x - all.min.x) / kGridCell) + 1;
    m_gridNz = static_cast<int>((all.max.z - all.min.z) / kGridCell) + 1;
    m_grid.assign(static_cast<std::size_t>(m_gridNx) * static_cast<std::size_t>(m_gridNz), {});
    for (std::size_t r = 0; r < m_rooms.size(); ++r)
        for (std::size_t i = 0; i < m_rooms[r].polys.size(); ++i) {
            const Aabb& b = m_rooms[r].polys[i].box;
            const int x0 = std::clamp(static_cast<int>((b.min.x - m_gridMinX) / kGridCell), 0, m_gridNx - 1);
            const int x1 = std::clamp(static_cast<int>((b.max.x - m_gridMinX) / kGridCell), 0, m_gridNx - 1);
            const int z0 = std::clamp(static_cast<int>((b.min.z - m_gridMinZ) / kGridCell), 0, m_gridNz - 1);
            const int z1 = std::clamp(static_cast<int>((b.max.z - m_gridMinZ) / kGridCell), 0, m_gridNz - 1);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    m_grid[static_cast<std::size_t>(z) * static_cast<std::size_t>(m_gridNx) + static_cast<std::size_t>(x)]
                        .push_back({static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(i)});
        }
}

void Mm1Level::collect(const int* /*rooms*/, int /*count*/, const Vec3& centre, float radius,
                       phys::LevelBound& out) const {
    out.clear();
    if (m_grid.empty())
        return;
    const Vec3 r{radius, radius, radius};
    // The polygons of every grid cell the sphere's box touches, each once.
    const int x0 = std::clamp(static_cast<int>((centre.x - r.x - m_gridMinX) / kGridCell), 0, m_gridNx - 1);
    const int x1 = std::clamp(static_cast<int>((centre.x + r.x - m_gridMinX) / kGridCell), 0, m_gridNx - 1);
    const int z0 = std::clamp(static_cast<int>((centre.z - r.z - m_gridMinZ) / kGridCell), 0, m_gridNz - 1);
    const int z1 = std::clamp(static_cast<int>((centre.z + r.z - m_gridMinZ) / kGridCell), 0, m_gridNz - 1);
    std::vector<PolyRef> refs;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const auto& cell =
                m_grid[static_cast<std::size_t>(z) * static_cast<std::size_t>(m_gridNx) + static_cast<std::size_t>(x)];
            refs.insert(refs.end(), cell.begin(), cell.end());
        }
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    int total = 0;
    for (const PolyRef& ref : refs) {
        const Poly& p = m_rooms[ref.room].polys[ref.index];
        if (p.box.max.x < centre.x - r.x || p.box.min.x > centre.x + r.x || p.box.max.y < centre.y - r.y ||
            p.box.min.y > centre.y + r.y || p.box.max.z < centre.z - r.z || p.box.min.z > centre.z + r.z)
            continue;
        // The sphere against the polygon's plane.
        const float dist = (centre - p.v[0]).dot(p.normal);
        if (std::fabs(dist) > radius)
            continue;
        if (dist < 0.0f && std::fabs(p.normal.y) < 0.7f && p.thickness >= 1.0f) {
            // OpenMM2: a wall seen from its back (MM1's ramp walls, e.g. Chicago's off ramp on the
            // right of the road, face away from the road) still holds the car in: offered with its
            // normal turned towards the car and its corners turned round to stay counter-clockwise.
            std::array<Vec3, 4> v = p.v;
            std::reverse(v.begin(), v.begin() + p.count);
            out.addPolygon(v.data(), p.count, p.normal * -1.0f, p.material);
        } else {
            out.addPolygon(p.v.data(), p.count, p.normal, p.material);
        }
        if (++total >= kMaxPolygons)
            break;
    }
}

phys::PolygonSoup Mm1Level::buildSoup(const phys::MaterialTable& materials) const {
    phys::SoupGeometry g;
    g.materialNames = m_soupNames;
    for (const auto& room : m_rooms)
        for (const Poly& p : room.polys) {
            phys::SoupGeometry::Poly sp;
            sp.count = static_cast<std::uint8_t>(p.count);
            for (int i = 0; i < p.count; ++i) {
                sp.v[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(g.vertices.size());
                g.vertices.push_back(p.v[static_cast<std::size_t>(i)]);
            }
            sp.material = p.soupMaterial;
            g.polys.push_back(sp);
        }
    phys::PolygonSoup soup;
    soup.add(g, Mat34::identity(), materials);
    return soup;
}

} // namespace mm2::game
