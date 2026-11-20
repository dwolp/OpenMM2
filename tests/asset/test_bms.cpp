// MM1 mesh sets (.bms), read as Open1560's agiMeshSet::BinaryLoad.
#include "asset/Bms.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <utility>
#include <string_view>
#include <vector>

using namespace mm2;
using namespace mm2::asset;

namespace {

struct Writer {
    std::vector<std::byte> d;
    Writer& u8(std::uint8_t v) {
        d.push_back(std::byte{v});
        return *this;
    }
    Writer& u16(std::uint16_t v) { return u8(v & 0xFF).u8(v >> 8); }
    Writer& u32(std::uint32_t v) { return u16(v & 0xFFFF).u16(v >> 16); }
    Writer& f32(float v) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        return u32(u);
    }
    Writer& name(std::string_view s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            u8(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
        return *this;
    }
};

// A unit quad (texture 1) plus a triangle (untextured), with uv and normals.
std::vector<std::byte> sampleMesh() {
    Writer w;
    w.u32(0x4D534833u).u32(0).u32(0).u32(0); // "3HSM" + 12 unused bytes
    w.u32(4).u32(5).u32(2).u32(8);           // vertices, adjuncts, surfaces, indices
    w.f32(1.5f).f32(2.25f).f32(1.5f);        // radius, radius^2, box radius
    w.u8(1).u8(0x03).u16(0).u32(0);          // 1 texture, uv | normal, pad, cache size
    w.name("ROAD", 32).u8(0x06).u8(0).u8(0).u8(0).u32(1).f32(2.0f).u32(0xFF112233);
    // Positions: a unit square, (0,0,0) (1,0,0) (1,0,1) (0,0,1).
    for (auto [x, z] : {std::pair{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}})
        w.f32(x).f32(0).f32(z);
    for (int i = 0; i < 5; ++i) // normals: index 2 is +Y in the table
        w.u8(2);
    for (int i = 0; i < 5; ++i)
        w.f32(0.25f * i).f32(0.5f);
    for (int p : {0, 1, 2, 3, 0}) // adjunct -> position
        w.u16(static_cast<std::uint16_t>(p));
    w.u8(1).u8(0);                              // surface textures
    w.u16(1).u16(2).u16(3).u16(4);              // quad (corner 3 nonzero)
    w.u16(0).u16(1).u16(2).u16(0);              // triangle
    return w.d;
}

} // namespace

TEST(Bms, ParsesHeaderTexturesAndAdjuncts) {
    std::string err;
    auto m = parseBms(sampleMesh(), &err);
    ASSERT_TRUE(m) << err;
    EXPECT_EQ(m->flags, BmsFlag::Uv | BmsFlag::Normal);
    ASSERT_EQ(m->textures.size(), 1u);
    EXPECT_EQ(m->textures[0].name, "ROAD");
    EXPECT_EQ(m->textures[0].flags, 0x06);
    EXPECT_EQ(m->textures[0].color, 0xFF112233u);
    EXPECT_EQ(m->positions.size(), 4u);
    EXPECT_TRUE(m->boundingBox.empty()); // only stored from 16 vertices up
    EXPECT_EQ(m->adjunctCount(), 5u);
    ASSERT_EQ(m->surfaces.size(), 2u);
    EXPECT_TRUE(m->surfaces[0].quad());
    EXPECT_FALSE(m->surfaces[1].quad());
    EXPECT_EQ(m->triangleCount(), 3u);
    EXPECT_EQ(unpackBmsNormal(2).y, 1.0f);
}

TEST(Bms, ConvertsWithMm1QuadSplit) {
    auto m = parseBms(sampleMesh());
    ASSERT_TRUE(m);
    const auto c = bmsToPkgMesh(*m);
    // Texture 1 -> material 0; the untextured triangle -> an extra last material.
    ASSERT_EQ(c.materials.size(), 2u);
    EXPECT_EQ(c.materials[0].texture, "ROAD");
    EXPECT_TRUE(c.materials[1].texture.empty());
    ASSERT_EQ(c.mesh.sections.size(), 2u);
    EXPECT_EQ(c.mesh.sections[0].shaderIndex, 1u); // slot 0 (untextured) first
    EXPECT_EQ(c.mesh.sections[1].shaderIndex, 0u);
    // The quad's corners are adjuncts 1,2,3,4 (positions 1,2,3,0). MM1 splits
    // corners (1,2,3) and (1,3,0): positions 2,3,0 then 2,0,1.
    const auto& quad = c.mesh.sections[1].packets.at(0);
    ASSERT_EQ(quad.indices.size(), 6u);
    auto px = [&](int i) { return quad.vertices[quad.indices[i]].position; };
    const std::array<Vec3, 6> expected{Vec3{1, 0, 1}, {0, 0, 1}, {0, 0, 0}, {1, 0, 1}, {0, 0, 0}, {1, 0, 0}};
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(px(i).x, expected[i].x) << i;
        EXPECT_EQ(px(i).z, expected[i].z) << i;
    }
    EXPECT_EQ(quad.vertices[0].normal.y, 1.f);
}

TEST(Bms, RejectsCorruptMeshes) {
    std::string err;
    auto bad = sampleMesh();
    bad[0] = std::byte{0};
    EXPECT_FALSE(parseBms(bad, &err));

    auto truncated = sampleMesh();
    truncated.resize(truncated.size() - 3);
    EXPECT_FALSE(parseBms(truncated, &err));

    // A surface corner past the adjunct table.
    auto range = sampleMesh();
    range[range.size() - 16] = std::byte{9};
    EXPECT_FALSE(parseBms(range, &err));
}
