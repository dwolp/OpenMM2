#pragma once

// Midtown Madness 1 city object lists: city/<city>.bng (props and other
// placed objects) and city/<city>.fcd (facades: the building fronts, walls,
// tunnels and rails along the streets). MM2 never reads these; OpenMM2
// reads them to bring MM1's Chicago into the fork. See docs/formats/mm1city.md.
//
// Read as MM1's mmCullCity::LoadBangers / LoadFacades do (Open1560
// code/midtown/game.asm, GPL-3.0): a u32 count, then fixed records each
// followed by a NUL-terminated mesh name, all handed to mmCullCity::AddInstance.

#include "core/Math.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm2::city {

// INST_INIT_FLAG_* (Open1560 mmcity/inst.h).
namespace Mm1InstFlag {
inline constexpr std::uint16_t Static = 0x1;     // part of the building chain; else an unhit banger
inline constexpr std::uint16_t Shear = 0x2;      // mmShearInstance
inline constexpr std::uint16_t Building = 0x4;   // mmBuildingInstance (with Static, without Shear)
inline constexpr std::uint16_t FacadeLeft = 0x8; // facades: draw the LEFT side
inline constexpr std::uint16_t FacadeRight = 0x10;
inline constexpr std::uint16_t FacadeTop = 0x20;
inline constexpr std::uint16_t Glow = 0x40;
inline constexpr std::uint16_t Upper = 0x200;
inline constexpr std::uint16_t FacadeBack = 0x400;
inline constexpr std::uint16_t Terrain = 0x800;
} // namespace Mm1InstFlag

// One .bng record (0x1C bytes + name): AddInstance(cell, name, 0, flags,
// &position, &ahead, nullptr, 0). `ahead` is a point 1 m in front of the
// object (verified: |ahead - position| = 1 for every Chicago record).
struct Mm1Banger {
    std::uint16_t cell = 0;
    std::uint16_t flags = 0;
    Vec3 position;
    Vec3 ahead;
    std::string name;
};

// One .fcd record (0x2C bytes + name): AddInstance(cell, name, 0, flags,
// &start, &end, &sides, width). The front runs from `start` to `end`;
// `width` is the facade mesh's own width (its FACADE mesh spans x 0..width),
// so the mesh is stretched by |end - start| / width (MatrixFromPoints).
// `sides` x and y go to the LEFT and RIGHT side quads (mmFacadeQuad), with
// `sides.z`: inferred to be the heights of the neighbouring buildings the
// sides are clipped against (-start.y when there is none).
struct Mm1Facade {
    std::uint16_t cell = 0;
    std::uint16_t flags = 0;
    Vec3 start;
    Vec3 end;
    Vec3 sides;
    float width = 0;
    std::string name;
};

std::optional<std::vector<Mm1Banger>> parseMm1Bng(std::span<const std::byte> data, std::string* error = nullptr);
std::optional<std::vector<Mm1Facade>> parseMm1Fcd(std::span<const std::byte> data, std::string* error = nullptr);

// MM1's MatrixFromPoints: x along end - start, scaled by |end - start| /
// width; y up; z = x cross y (unscaled); the origin at start, lowered to
// end.y when that is lower.
Mat34 mm1MatrixFromPoints(const Vec3& start, const Vec3& end, float width);

// mmBuildingInstance::Init(name, a, b, c), for .fcd records flagged Static |
// Building (not Shear): rows b - a, +Y, c - a, unnormalised, origin a. Its
// meshes are the FACADE and GRND groups. (mmCullCity::AddInstance: Static
// and Shear make an mmShearInstance, Static and Building a building, other
// Static records a facade, the rest bangers.)
Mat34 mm1BuildingMatrix(const Vec3& a, const Vec3& b, const Vec3& c);

// mmShearInstance::Init(name, start, end, width), for .fcd records flagged
// Static | Shear: x = unit(end - start) scaled by |end - start| / width (its
// y keeps the slope: the piece shears, its verticals stay vertical); y up;
// z = (-x.z, 0, x.x) of the unit direction; the origin exactly at start (no
// lowering, unlike MatrixFromPoints). Its meshes are the model's own H/M/L/VL,
// else its FACADE group, else its BLDG group (InitMeshes' order).
Mat34 mm1ShearMatrix(const Vec3& start, const Vec3& end, float width);

} // namespace mm2::city
