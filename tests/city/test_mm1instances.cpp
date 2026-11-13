// MM1 city object lists (.bng, .fcd) and their placement matrices.
#include "city/Mm1City.h"
#include "city/Mm1Instances.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

using namespace mm2;
using namespace mm2::city;

namespace {

struct Writer {
    std::vector<std::byte> d;
    Writer& u16(std::uint16_t v) {
        d.push_back(std::byte(v & 0xFF));
        d.push_back(std::byte(v >> 8));
        return *this;
    }
    Writer& u32(std::uint32_t v) { return u16(v & 0xFFFF).u16(v >> 16); }
    Writer& f32(float v) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        return u32(u);
    }
    Writer& vec(float x, float y, float z) { return f32(x).f32(y).f32(z); }
    Writer& str(std::string_view s) {
        for (char c : s)
            d.push_back(std::byte(c));
        d.push_back(std::byte{0});
        return *this;
    }
};

} // namespace

TEST(Mm1Instances, ParsesBangers) {
    Writer w;
    w.u32(2);
    w.u16(7).u16(0x18).vec(1, 0, 2).vec(2, 0, 2).str("tppmtr");
    w.u16(9).u16(0x58).vec(-3, 0.5f, 4).vec(-3, 0.5f, 5).str("tpltst");
    std::string err;
    auto b = parseMm1Bng(w.d, &err);
    ASSERT_TRUE(b) << err;
    ASSERT_EQ(b->size(), 2u);
    EXPECT_EQ((*b)[0].cell, 7);
    EXPECT_EQ((*b)[0].flags, 0x18);
    EXPECT_EQ((*b)[0].name, "tppmtr");
    EXPECT_EQ((*b)[1].ahead.z, 5.f);
}

TEST(Mm1Instances, ParsesFacadesAndRejectsTrailingBytes) {
    Writer w;
    w.u32(1).u16(358).u16(0x21).vec(0, 0.14f, 0).vec(24, 0.14f, 0).vec(60, -0.14f, 0).f32(23.99f).str("dfbldg04");
    std::string err;
    auto f = parseMm1Fcd(w.d, &err);
    ASSERT_TRUE(f) << err;
    ASSERT_EQ(f->size(), 1u);
    EXPECT_EQ((*f)[0].name, "dfbldg04");
    EXPECT_EQ((*f)[0].sides.x, 60.f);
    EXPECT_FLOAT_EQ((*f)[0].width, 23.99f);

    // A text-mode CR LF conversion (the bug once in mm2tool cat) leaves bytes over.
    w.d.push_back(std::byte{0x0D});
    EXPECT_FALSE(parseMm1Fcd(w.d, &err));
    // A truncated record.
    Writer t;
    t.u32(1).u16(1).u16(1).vec(0, 0, 0);
    EXPECT_FALSE(parseMm1Fcd(t.d, &err));
}

TEST(Mm1Instances, MatrixFromPointsStretchesAlongTheFront) {
    // A front about 12 m long along +Z, for a mesh 24 m wide.
    const Mat34 m = mm1MatrixFromPoints({10, 2, 5}, {10, 1, 17}, 24.0f);
    EXPECT_NEAR(m.m0.x, 0.0f, 1e-6f);
    EXPECT_NEAR(m.m0.mag(), std::sqrt(145.0f) / 24.0f, 1e-5f); // |end - start| / width
    EXPECT_EQ(m.m1.y, 1.0f);
    // z = unit(x) cross y: for x along +Z (and slightly down) z points -X.
    EXPECT_LT(m.m2.x, -0.99f);
    // The origin is the start, lowered to the end's height.
    EXPECT_EQ(m.m3.x, 10.f);
    EXPECT_EQ(m.m3.y, 1.f);
    // The mesh's far corner (width, 0, 0) lands on the end point in x/z.
    const Vec3 far = m.transform({24.0f, 0.0f, 0.0f});
    EXPECT_NEAR(far.z, 17.0f, 1e-4f);
}

TEST(Mm1Instances, BuildingMatrixUsesThePointsAsAxes) {
    const Mat34 m = mm1BuildingMatrix({1, 0, 1}, {3, 0, 1}, {1, 0, -2});
    EXPECT_EQ(m.m0.x, 2.f);
    EXPECT_EQ(m.m2.z, -3.f);
    EXPECT_EQ(m.m1.y, 1.f);
    EXPECT_EQ(m.m3.x, 1.f);
}

TEST(Mm1City, ParsesCellsWithAndWithoutTags) {
    int maxCells = 0;
    std::string err;
    auto cells = parseMm1Cells("3\r\n10\r\n1,460,0,0\r\n2,462,0,1,8\r\n9,448\r\n", &maxCells, &err);
    ASSERT_TRUE(cells) << err;
    EXPECT_EQ(maxCells, 11); // LoadCells: the max id + 1
    ASSERT_EQ(cells->size(), 3u);
    EXPECT_EQ((*cells)[0].cullFlags, 460);
    ASSERT_EQ((*cells)[1].visitTags.size(), 1u);
    EXPECT_EQ((*cells)[1].visitTags[0], 8);
    EXPECT_EQ((*cells)[2].index, 9);
    EXPECT_FALSE(parseMm1Cells("2\n5\n1,0\n9,0\n", nullptr, &err)); // id above the max
    EXPECT_FALSE(parseMm1Cells("5\n10\n1,0\n", nullptr, &err));     // fewer lines than counted
}

TEST(Mm1City, ParsesPortalsIntoPolygons) {
    Writer w;
    w.u32(0).u32(2);
    // Two edges: flags 1, cells 3 and 4, 5 m high, from (0,0,0) to (10,0,0).
    w.d.push_back(std::byte{1});
    w.d.push_back(std::byte{2});
    w.u16(0).u16(3).u16(4).f32(5.0f).vec(0, 0, 0).vec(10, 0, 0);
    // Three edges: an extra corner follows the record.
    w.d.push_back(std::byte{3});
    w.d.push_back(std::byte{3});
    w.u16(0).u16(4).u16(5).f32(2.0f).vec(0, 0, 0).vec(4, 0, 0).vec(2, -1, 0);
    std::string err;
    auto p = parseMm1Portals(w.d, &err);
    ASSERT_TRUE(p) << err;
    ASSERT_EQ(p->size(), 2u);
    ASSERT_EQ((*p)[0].polygon.size(), 4u);
    EXPECT_EQ((*p)[0].polygon[0].y, 5.f); // the top edge first
    EXPECT_EQ((*p)[0].polygon[2].x, 10.f);
    ASSERT_EQ((*p)[1].polygon.size(), 5u);
    EXPECT_EQ((*p)[1].polygon[3].y, -1.f); // the extra corner before the start
    EXPECT_EQ((*p)[1].cell2, 5);

    w.d.push_back(std::byte{0});
    EXPECT_FALSE(parseMm1Portals(w.d, &err)); // trailing bytes
}
