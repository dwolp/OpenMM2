#pragma once

// OpenMM2 Enhanced (not in MM2): a Midtown Madness 1 city (Chicago) as the
// collision manager sees it. CityLevel hands its phys::Level queries to this
// when the city is MM1's. Rooms are MM1's cells:
//
//   * findRoom: the cell whose footprint in bnd/<city>_hitid.bnd (one
//     polygon set per cell, asRenderWeb::LoadHitId) holds the point, the
//     nearest below it where footprints overlap (bridges, lower levels).
//   * neighbors / touchedNeighbors: the cells across the city's portals.
//   * collect: the cell's own bound, bnd/<city>city/bound<N>.bnd
//     (asRenderWeb::LoadRoomBounds; Chicago's construction site and Wrigley
//     Field cells use dl60_bnd and dl24_bnd).
//
// MM1's physics materials (mtl/physics.db, 1-based in the polygons) map to
// MM2's by name: grass, dirt, water, cobblestone, else MM2's default.
// Inferred and approximate: MM1's own collision (mmBoundTemplate's grid and
// edge tests) is not ported; the polygons go through MM2's.

#include "asset/Mm1Bound.h"
#include "city/Mm1City.h"
#include "phys/Level.h"
#include "phys/PolygonSoup.h"
#include "vfs/Vfs.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::game {

class Mm1Level {
public:
    // `managerIndex` gives the 0-based index of an MM2 material by name in
    // the level's material manager (-1 when it has none): a polygon's
    // material byte is that + 1 (lvlLevelBound::GetMaterial).
    Mm1Level(const city::Mm1City& city, const vfs::Vfs& vfs,
             const std::function<int(std::string_view)>& managerIndex);

    int findRoom(const Vec3& position, int hint) const;
    int neighbors(int* out, int max, int room) const;
    int touchedNeighbors(int* out, int max, int room, const Vec3& centre, float radius) const;
    void collect(const int* rooms, int count, const Vec3& centre, float radius, phys::LevelBound& out) const;
    // Every cell's polygons, for the wheels' probe geometry.
    phys::PolygonSoup buildSoup(const phys::MaterialTable& materials) const;

    int roomCount() const { return static_cast<int>(m_rooms.size()); }
    int polygonCount() const { return m_polygonCount; }

    // The MM2 material name an MM1 physics material stands for.
    static std::string_view mm2MaterialFor(std::string_view mm1Name);

private:
    struct Poly {
        std::array<Vec3, 4> v{};
        int count = 3;
        Vec3 normal;
        std::uint8_t material = 0; // the manager's index + 1
        std::uint8_t soupMaterial = 0; // index into m_soupNames
        Aabb box;
    };
    struct Room {
        std::vector<Poly> polys;
        std::vector<std::pair<int, std::array<Vec3, 2>>> portals; // neighbour, bottom edge
    };
    // The generated building walls: the probe's distance beyond an edge,
    // and how far below and above the edge the wall reaches.
    static constexpr float kWallProbe = 0.3f;
    static constexpr float kWallBelow = 1.0f;
    static constexpr float kWallHeight = 40.0f;

    // A uniform grid over every polygon of every cell (OpenMM2): collect()
    // takes the polygons near a sphere from it whatever cell they belong to,
    // since MM1's polygons are listed under one cell but lie across cell
    // borders (the portal graph alone leaves gaps there).
    struct PolyRef {
        std::uint32_t room, index;
        bool operator<(const PolyRef& o) const { return room != o.room ? room < o.room : index < o.index; }
        bool operator==(const PolyRef& o) const { return room == o.room && index == o.index; }
    };
    static constexpr float kGridCell = 16.0f;
    void buildGrid();
    float m_gridMinX = 0.0f, m_gridMinZ = 0.0f;
    int m_gridNx = 0, m_gridNz = 0;
    std::vector<std::vector<PolyRef>> m_grid;

    std::vector<Room> m_rooms; // by cell index
    const city::Mm1CellLocator* m_locator = nullptr; // the city's (CityData outlives the level)
    std::vector<std::string> m_soupNames;
    int m_polygonCount = 0;
};

} // namespace mm2::game
