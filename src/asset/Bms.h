#pragma once

// Midtown Madness 1 mesh sets (bms/<object>/<part>_<lod>.bms). See
// docs/formats/bms.md. MM2 never reads these; OpenMM2 reads them to bring
// MM1's Chicago into the fork.
//
// Layout after Open1560's agiMeshSet::BinaryLoad (code/midtown/agiworld/
// meshload.cpp, GPL-3.0). A mesh set has positions, and "adjuncts": the
// per-corner records (position index plus optional normal, uv and colour)
// that the surfaces (triangles or quads) index.

#include "asset/Pkg.h"
#include "core/Math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm2::asset {

namespace BmsFlag {
inline constexpr std::uint8_t Uv = 0x01;     // MESH_SET_UV: adjuncts carry texture coordinates
inline constexpr std::uint8_t Normal = 0x02; // MESH_SET_NORMAL: adjuncts carry packed normals
inline constexpr std::uint8_t Color = 0x04;  // MESH_SET_CPV: adjuncts carry a colour per vertex
inline constexpr std::uint8_t Offset = 0x08; // MESH_SET_OFFSET
inline constexpr std::uint8_t Planes = 0x10; // MESH_SET_PLANES: a plane equation per surface
} // namespace BmsFlag

// agiTexParameters, 0x30 bytes.
struct BmsTexture {
    std::string name; // texture base name; empty for an untextured slot
    std::uint8_t flags = 0; // agiTexParameters::Alpha 0x1, WrapU 0x2, WrapV 0x4, NoMipMaps 0x10, Chromakey 0x40
    std::uint32_t props = 0;
    float snow = 0; // Open1560: "Snowable ? 4.0f : 2.0f"; meaning unconfirmed
    std::uint32_t color = 0;
};

struct BmsSurface {
    std::array<std::uint16_t, 4> corners{}; // adjunct indices; corners[3] == 0 marks a triangle
    std::uint8_t texture = 0;                // 1-based into BmsMesh::textures; 0 = untextured
    bool quad() const { return corners[3] != 0; }
};

struct BmsMesh {
    std::uint8_t flags = 0; // BmsFlag
    float radius = 0, radiusSqr = 0, boxRadius = 0;
    std::vector<BmsTexture> textures; // textures[0] is slot 1
    std::vector<Vec3> positions;
    std::vector<Vec3> boundingBox; // 8 corners when positions.size() >= 16

    // Per adjunct (all the same size).
    std::vector<std::uint16_t> adjunctPosition;
    std::vector<std::uint8_t> adjunctNormal; // index into unpackBmsNormal's table
    std::vector<Vec2> adjunctUv;
    std::vector<std::uint32_t> adjunctColor;

    std::vector<Vec4> planes; // per surface, with BmsFlag::Planes
    std::vector<BmsSurface> surfaces;

    std::size_t adjunctCount() const { return adjunctPosition.size(); }
    std::size_t triangleCount() const;
    Aabb bounds() const;
};

// UnpackNormal (Open1560 agiworld/packnorm.cpp): MM1 stores normals as an
// index into a fixed table of 198 directions. Out-of-range indices give +Y.
Vec3 unpackBmsNormal(std::uint8_t index);

std::optional<BmsMesh> parseBms(std::span<const std::byte> data, std::string* error = nullptr);

// The mesh as a Pkg-style mesh, so the MM2 renderer can draw it: one section
// per texture slot (shaderIndex = slot - 1; untextured surfaces use the last
// index, materials.size() - 1, when there are any) and triangle lists. Quads
// split as MM1's renderer splits them: (1, 2, 3) and (1, 3, 0).
struct BmsConverted {
    PkgMesh mesh;
    std::vector<PkgMaterial> materials;
};
BmsConverted bmsToPkgMesh(const BmsMesh& bms);

} // namespace mm2::asset
