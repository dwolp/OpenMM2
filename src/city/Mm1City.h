#pragma once

// A Midtown Madness 1 city (Chicago), as MM1's mmCullCity and asRenderWeb
// load it: cells (the rooms, each with prebuilt meshes), the portals between
// them, the facades and the props. MM2 has no such city; OpenMM2 reads it
// for the Enhanced fork. See docs/formats/mm1city.md.
//
// After Open1560 (GPL-3.0): asRenderWeb::LoadCells / LoadPortals
// (mmcity/renderweb.cpp) and mmCellRenderer::Init (game.asm).

#include "city/Mm1Instances.h"
#include "core/Math.h"
#include "vfs/Vfs.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::city {

// Cells below this use the landmark meshes (bms/<city>lm/) when MM1's
// lightmaps are on; the rest, and all cells otherwise, bms/<city>city/
// (renderweb.cpp CITY_MESH_START).
inline constexpr int kMm1CityMeshStart = 200;

// mmCellRenderer::Init's mesh groups, in its Meshes[] order:
// CULL<n>_H (or plain CULL<n>, required), _M, _L, _A, then the second set
// _H2, _M2, _L2, _A2.
inline constexpr std::array<std::string_view, 8> kMm1CellGroups = {"H", "M", "L", "A", "H2", "M2", "L2", "A2"};

struct Mm1Cell {
    int index = 0;
    int cullFlags = 0;
    int roomFlags = 0;
    std::vector<int> visitTags;
};

// One .ptl record (PtlPortal, 0x24 bytes, plus a Vec3 for three-edge
// portals): the opening between two cells, as the polygon LoadPortals builds
// (the top edge `height` above the bottom one).
struct Mm1Portal {
    std::uint8_t flags = 0; // asPortalEdge: 0x1 enabled, 0x2 open, 0x4 half open, 0x8 forward plane
    int edgeCount = 2;      // 2 or 3 (as stored)
    int cell1 = 0, cell2 = 0;
    float height = 0;
    std::vector<Vec3> polygon; // 4 or 5 corners
};

// One piece of a city/<city>.gizmo bridge set (mmBridgeSet::ReadEntry):
// "<tab><model>,<ignored>,x0,y0,z0,x1,y1,z1". The leaves (tpdrawbridge*)
// hinge at `position` about the axis towards `axisPoint` and reach 32.5 m
// toward the matrix's -Z (Chicago's: two leaves across each 65 m river
// gap); the gates (tpcrossgate*) and their bases sit beside them.
struct Mm1GizmoPart {
    std::string model; // lower case
    Vec3 position;
    Vec3 axisPoint;
    bool leaf() const { return model.starts_with("tpdrawbridge"); }
    // Inferred from the data (mmAnimDOF::Init is unported): x along the
    // hinge, y up, z = x cross y, at `position`.
    Mat34 matrix() const;
};
// mmBridgeMgr::Init: blocks named on a line of their own ("DrawBridge3"),
// closed by the same name again, holding the pieces.
struct Mm1BridgeSet {
    std::string name;
    std::vector<Mm1GizmoPart> parts;
};
std::vector<Mm1BridgeSet> parseMm1Gizmo(std::string_view text);

// asRenderWeb::LoadCells: "<count>\n<max id>\n" then
// "id,cullFlags[,roomFlags,tagCount,tag...]" per cell.
std::optional<std::vector<Mm1Cell>> parseMm1Cells(std::string_view text, int* maxCells = nullptr,
                                                  std::string* error = nullptr);
// asRenderWeb::LoadPortals: i32 0, i32 count, then the records.
std::optional<std::vector<Mm1Portal>> parseMm1Portals(std::span<const std::byte> data, std::string* error = nullptr);

// One row of MM1's tune/lighting.csv (mmCullCity's lights and fog for a
// time of day and weather). Headings and pitches in radians, light colours
// 0..1, fog and shadow colours 0..255.
struct Mm1Lighting {
    int timeOfDay = 0; // 0 morning, 1 noon, 2 sunset, 3 night
    int weather = 0;   // 0 clear, 1 fog, 2 rain, 3 snow (mmEnvSetup's c, f, r, s)
    float sunHeading = 0, sunPitch = 0;
    Vec3 sunColor;
    float fill1Heading = 0, fill1Pitch = 0;
    Vec3 fill1Color;
    float fill2Heading = 0, fill2Pitch = 0;
    Vec3 fill2Color;
    Vec3 ambient;
    float fogEnd = 0;
    std::array<std::uint8_t, 3> fogColor{};
    float shadowAlpha = 0;
    std::array<std::uint8_t, 3> shadowColor{};
};
// The table: a header line, then "TOD,weather,sun heading,..." rows.
std::vector<Mm1Lighting> parseMm1Lighting(std::string_view text);

struct Mm1City {
    std::string name; // "chicago"
    int maxCells = 0;
    std::vector<Mm1Cell> cells;
    std::vector<Mm1Portal> portals;
    std::vector<Mm1Facade> facades;
    std::vector<Mm1Banger> bangers;
    std::vector<Mm1BridgeSet> bridges; // city/<name>.gizmo
    std::vector<Mm1Lighting> lighting;  // tune/lighting.csv (MM1's, for every city)
    // city/<name>.ext: x min, z min, x max, z max.
    std::optional<std::array<float, 4>> extent;
    std::vector<std::string> warnings;

    // The bms/ directory of a cell's meshes (see kMm1CityMeshStart).
    std::string cellMeshDir(int index, bool landmarks = true) const;
    // A cell's mesh file for group `group` (kMm1CellGroups), relative to
    // the VFS root: "mm1/bms/chicagocity/cull1000_h.bms".
    std::string cellMeshPath(int index, std::string_view group, bool landmarks = true) const;
};

// Loads city `name` from the MM1 files under vfs::kMm1Prefix. The cells and
// portals are required; the rest add warnings when missing.
std::optional<Mm1City> loadMm1City(const vfs::Vfs& vfs, std::string_view name, std::string* error = nullptr);

// True when the VFS holds MM1 city `name` (its .cells file).
bool hasMm1City(const vfs::Vfs& vfs, std::string_view name);

} // namespace mm2::city
