#pragma once

// Midtown Madness 1 collision: binary bound templates ("2DNB",
// bnd/<city>city/bound<N>.bnd per cell and bnd/<city>_hitid.bnd for the
// whole city) and the physics materials they name (mtl/physics.db). MM2 has
// neither (its binary bounds are another format, see bnd.md); OpenMM2 reads
// them for the Enhanced fork's MM1 cities. See docs/formats/mm1city.md.
//
// After Open1560 (GPL-3.0): mmBoundTemplate and mmPolygon (mmdyna/
// bndtmpl.h, poly.h) and mmBoundTemplate::Load's binary branch (game.asm).

#include "core/Math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm2::asset {

// mmPolygon, 0x4C bytes.
struct Mm1BoundPolygon {
    std::uint16_t room = 0;    // the cell the polygon belongs to
    std::uint8_t material = 0; // 1-based into mtl/physics.db; 0 = none
    std::uint8_t flags = 0;    // 0x3 edge projection axis, 0x4 quad, 0x8 flipped winding (inferred)
    std::array<std::int16_t, 4> vertices{};
    Vec3 normal; // the plane: normal . p + d == 0
    float d = 0;
    bool quad() const { return (flags & 0x4) != 0; }
};

struct Mm1Bound {
    Vec3 offset;                 // the 12 bytes after the magic (zero in Chicago's)
    std::array<int, 3> grid{};   // XDim, YDim, ZDim of the polygon lookup grid
    Vec3 centre;
    float radius = 0;
    Aabb box;
    std::vector<Vec3> vertices; // world space
    std::vector<Mm1BoundPolygon> polygons;
};

// The header, vertices and polygons; the lookup grid and edge tables that
// follow are not read (OpenMM2 builds its own lookups).
std::optional<Mm1Bound> parseMm1Bound(std::span<const std::byte> data, std::string* error = nullptr);

// One mtl/physics.db record: a 32-byte name and 14 big-endian floats.
// Inferred: friction first, then elasticity (TBPARK03GRASS has 0.5 where
// the rest have 1.0); the others are unidentified.
struct Mm1PhysicsMaterial {
    std::string name;
    std::array<float, 14> values{};
    float friction() const { return values[0]; }
    float elasticity() const { return values[1]; }
};

std::optional<std::vector<Mm1PhysicsMaterial>> parseMm1PhysicsDb(std::span<const std::byte> data,
                                                                 std::string* error = nullptr);

} // namespace mm2::asset
