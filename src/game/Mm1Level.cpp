#include "game/Mm1Level.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
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
            // MM2's collector expects the corners counter-clockwise about
            // the normal; MM1's mostly are, the rest are turned round.
            const Vec3 cross = (poly.v[1] - poly.v[0]).cross(poly.v[2] - poly.v[0]);
            if (cross.dot(poly.normal) < 0.0f)
                std::reverse(poly.v.begin(), poly.v.begin() + poly.count);
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

    // The building walls. MM1's bounds hold the ground, kerbs and barriers
    // but no building fronts: the drivable area is the footprints' union, and
    // leaving it is hitting a building. Inferred, until MM1's own test
    // (mmBoundTemplate's 2D edges) is ported: every footprint edge with no
    // footprint just beyond it, at about its height, becomes a wall from
    // the edge up, collected with the footprint's cell.
    int walls = 0;
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
            const Vec3 probe = mid + out * kWallProbe;
            if (m_locator->footprintNear(probe, mid.y))
                continue;
            Poly wall;
            wall.count = 4;
            // Counter-clockwise seen from outside the drivable area... the
            // normal faces back into it.
            wall.normal = out * -1.0f;
            wall.v = {a + Vec3{0, -kWallBelow, 0}, a + Vec3{0, kWallHeight, 0}, b + Vec3{0, kWallHeight, 0},
                      b + Vec3{0, -kWallBelow, 0}};
            const Vec3 cross = (wall.v[1] - wall.v[0]).cross(wall.v[2] - wall.v[0]);
            if (cross.dot(wall.normal) < 0.0f)
                std::reverse(wall.v.begin(), wall.v.end());
            for (const auto& v : wall.v)
                wall.box.expand(v);
            m_rooms[static_cast<std::size_t>(f.cell)].polys.push_back(wall);
            ++walls;
            ++m_polygonCount;
        }
    }
    // The drawbridges' leaves, down (city/<city>.gizmo; mmBridgeSet's
    // opening is not ported): each leaf's bound, bnd/<model>_bnd.bnd in the
    // leaf's frame, placed and listed with the cells its polygons lie in
    // (the bridge cells whose footprints cover the decks).
    int bridgePolys = 0;
    for (const auto& set : city.bridges) {
        for (const auto& part : set.parts) {
            if (!part.leaf())
                continue;
            auto bytes = vfs.readAll(std::format("{}bnd/{}_bnd.bnd", prefix, part.model));
            if (!bytes)
                continue;
            std::string err;
            auto bound = asset::parseMm1Bound(*bytes, &err);
            if (!bound) {
                log::warn("mm1: bnd/{}_bnd.bnd: {}", part.model, err);
                continue;
            }
            const Mat34 m = part.matrix();
            for (const auto& p : bound->polygons) {
                Poly poly;
                poly.count = p.quad() ? 4 : 3;
                Vec3 centre{};
                for (int i = 0; i < poly.count; ++i) {
                    poly.v[static_cast<std::size_t>(i)] =
                        m.transform(bound->vertices[static_cast<std::size_t>(p.vertices[static_cast<std::size_t>(i)])]);
                    centre = centre + poly.v[static_cast<std::size_t>(i)];
                }
                centre = centre * (1.0f / static_cast<float>(poly.count));
                poly.normal = m.transformDir(p.normal);
                const Vec3 cross = (poly.v[1] - poly.v[0]).cross(poly.v[2] - poly.v[0]);
                if (cross.dot(poly.normal) < 0.0f)
                    std::reverse(poly.v.begin(), poly.v.begin() + poly.count);
                poly.material = p.material < toManager.size() ? toManager[p.material] : 0;
                poly.soupMaterial = p.material < toSoup.size() ? toSoup[p.material] : 0;
                for (int i = 0; i < poly.count; ++i)
                    poly.box.expand(poly.v[static_cast<std::size_t>(i)]);
                // The cells under the polygon's centre and its corners.
                std::vector<int> rooms;
                for (int i = -1; i < poly.count; ++i) {
                    const Vec3 at = i < 0 ? centre : poly.v[static_cast<std::size_t>(i)];
                    const int room = findRoom(at + Vec3{0, 0.5f, 0}, 0);
                    if (room > 0 && std::ranges::find(rooms, room) == rooms.end())
                        rooms.push_back(room);
                }
                for (int room : rooms)
                    m_rooms[static_cast<std::size_t>(room)].polys.push_back(poly);
                if (!rooms.empty()) {
                    ++bridgePolys;
                    ++m_polygonCount;
                }
            }
        }
    }
    log::info("mm1 collision: {} polygons in {} cells ({} without a bound; {} building walls, {} bridge "
              "polygons), {} footprints",
              m_polygonCount, city.cells.size(), missing, walls, bridgePolys, m_locator ? m_locator->footprints().size() : 0);
}

int Mm1Level::findRoom(const Vec3& position, int hint) const {
    return m_locator ? m_locator->find(position, hint) : 0;
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

void Mm1Level::collect(const int* rooms, int count, const Vec3& centre, float radius, phys::LevelBound& out) const {
    out.clear();
    const Vec3 r{radius, radius, radius};
    int total = 0;
    for (int k = 0; k < count && total < kMaxPolygons; ++k) {
        const int room = rooms[k];
        if (room <= 0 || static_cast<std::size_t>(room) >= m_rooms.size())
            continue;
        for (const Poly& p : m_rooms[static_cast<std::size_t>(room)].polys) {
            if (p.box.max.x < centre.x - r.x || p.box.min.x > centre.x + r.x || p.box.max.y < centre.y - r.y ||
                p.box.min.y > centre.y + r.y || p.box.max.z < centre.z - r.z || p.box.min.z > centre.z + r.z)
                continue;
            // The sphere against the polygon's plane.
            const float dist = (centre - p.v[0]).dot(p.normal);
            if (std::fabs(dist) > radius)
                continue;
            out.addPolygon(p.v.data(), p.count, p.normal, p.material);
            if (++total >= kMaxPolygons)
                break;
        }
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
