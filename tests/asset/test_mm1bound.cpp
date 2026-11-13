// MM1 collision bounds (2DNB) and physics materials.
#include "asset/Mm1Bound.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstring>
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
    Writer& f32(float v) { return u32(std::bit_cast<std::uint32_t>(v)); }
    Writer& vec(float x, float y, float z) { return f32(x).f32(y).f32(z); }
    Writer& be32(std::uint32_t v) { return u8(v >> 24).u8((v >> 16) & 0xFF).u8((v >> 8) & 0xFF).u8(v & 0xFF); }
};

// A unit floor quad (cell 7, material 74) and an upright triangle.
std::vector<std::byte> sampleBound() {
    Writer w;
    w.u32(0x424E4432u).vec(0, 0, 0);       // "2DNB", offset
    w.u32(1).u32(1).u32(1);                // grid
    w.vec(0.5f, 0, 0.5f).f32(1).f32(1);    // centre, radius, radius^2
    w.vec(0, 0, 0).vec(1, 1, 1);           // box
    w.u32(5).u32(2).u32(0).u32(0).u32(0);  // vertices, polygons, hot verts, edges
    w.f32(1).f32(1).u32(0).f32(1).u32(0);  // scales and table sizes
    w.vec(0, 0, 0).vec(1, 0, 0).vec(1, 0, 1).vec(0, 0, 1).vec(0, 1, 0);
    auto poly = [&](std::uint16_t room, std::uint8_t mtl, std::uint8_t flags, std::array<std::int16_t, 4> v,
                    float nx, float ny, float nz, float d) {
        w.u16(room).u8(mtl).u8(flags);
        for (auto i : v)
            w.u16(static_cast<std::uint16_t>(i));
        for (int i = 0; i < 12; ++i)
            w.f32(0); // plane edges
        w.vec(nx, ny, nz).f32(d);
    };
    poly(7, 74, 0x4, {0, 3, 2, 1}, 0, 1, 0, 0);
    poly(7, 0, 0x0, {0, 1, 4, 0}, 0, 0, 1, 0);
    return w.d;
}

} // namespace

TEST(Mm1Bound, ParsesVerticesAndPolygons) {
    std::string err;
    auto b = parseMm1Bound(sampleBound(), &err);
    ASSERT_TRUE(b) << err;
    EXPECT_EQ(b->vertices.size(), 5u);
    ASSERT_EQ(b->polygons.size(), 2u);
    EXPECT_TRUE(b->polygons[0].quad());
    EXPECT_FALSE(b->polygons[1].quad());
    EXPECT_EQ(b->polygons[0].room, 7);
    EXPECT_EQ(b->polygons[0].material, 74);
    EXPECT_EQ(b->polygons[0].normal.y, 1.0f);
    EXPECT_EQ(b->box.max.x, 1.0f);
}

TEST(Mm1Bound, RejectsBadIndicesAndMagic) {
    std::string err;
    auto bad = sampleBound();
    bad[0] = std::byte{0};
    EXPECT_FALSE(parseMm1Bound(bad, &err));
    auto range = sampleBound();
    range[0x70 + 5 * 12 + 4] = std::byte{9}; // first polygon's first vertex
    EXPECT_FALSE(parseMm1Bound(range, &err));
}

TEST(Mm1PhysicsDb, ReadsBigEndianRecords) {
    Writer w;
    w.be32(2);
    for (const char* name : {"SDWLK", "TBPARK03GRASS"}) {
        char buf[32] = {};
        std::strncpy(buf, name, sizeof(buf) - 1);
        for (char c : buf)
            w.u8(static_cast<std::uint8_t>(c));
        for (int i = 0; i < 14; ++i)
            w.be32(std::bit_cast<std::uint32_t>(i == 0 ? (name[0] == 'T' ? 0.5f : 1.0f) : 0.1f));
    }
    std::string err;
    auto m = parseMm1PhysicsDb(w.d, &err);
    ASSERT_TRUE(m) << err;
    ASSERT_EQ(m->size(), 2u);
    EXPECT_EQ((*m)[1].name, "TBPARK03GRASS");
    EXPECT_FLOAT_EQ((*m)[1].friction(), 0.5f);
    EXPECT_FLOAT_EQ((*m)[0].elasticity(), 0.1f);
}
