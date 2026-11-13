// MM1's AI road network (.bai) and its conversion to MM2's.
#include "city/Mm1AiMap.h"

#include <gtest/gtest.h>

#include <bit>
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
    Writer& f32(float v) { return u32(std::bit_cast<std::uint32_t>(v)); }
    Writer& vec(float x, float y, float z) { return f32(x).f32(y).f32(z); }
    Writer& name(std::string_view s) {
        for (std::size_t i = 0; i < 32; ++i)
            d.push_back(std::byte(i < s.size() ? s[i] : 0));
        return *this;
    }
};

// One path from intersection `src` to `sink` along x (z = 0), five vertices,
// one lane 2.5 m to its right and a sidewalk at 7.5 m.
void path(Writer& w, int id, int src, int sink, int oncoming, float dir, std::string_view light) {
    const int vc = 5;
    w.u16(static_cast<std::uint16_t>(id)).u16(vc).u16(1).u16(1); // id, vertices, lanes, sidewalks
    w.u16(0).u16(0);                                             // stop light index, intersection type
    for (int i = 0; i < 6; ++i)                                  // blocked, ped blocked, divided, flat, bridge, alley
        w.u16(i == 2 ? 1 : 0);
    w.f32(40).f32(15).name(light);
    w.u32(static_cast<std::uint32_t>(oncoming)).u32(0).u32(0);
    for (int i = 0; i < 2 * vc + vc; ++i)
        w.f32(0); // sub-section and centre offsets
    w.u32(static_cast<std::uint32_t>(sink)).u32(static_cast<std::uint32_t>(src));
    auto row = [&](float side) {
        for (int i = 0; i < vc; ++i) {
            const float x = dir > 0 ? 10.0f * i : 40.0f - 10.0f * i;
            const bool end = i == 0 || i == vc - 1;
            w.vec(x, 0, end ? 0.0f : side * dir);
        }
    };
    row(2.5f);  // lane
    row(7.5f);  // sidewalk
    row(0.0f);  // centre
    for (int k = 0; k < 4; ++k) // x axes, normals, z axes, sub-section directions
        for (int i = 0; i < vc; ++i)
            w.vec(0, k == 1 ? 1.0f : 0.0f, k == 0 ? -dir : 0.0f);
    row(10.0f); // outer edge
    row(5.0f);  // curb
    row(-5.0f); // far curb
    w.vec(0, 0, 0).vec(0, 0, 0);
    for (int i = 0; i < 15; ++i)
        w.f32(0);
}

std::vector<std::byte> sampleBai() {
    Writer w;
    w.u16(2).u16(2);
    path(w, 0, 0, 1, 1, 1.0f, "tplttrafc");
    path(w, 1, 1, 0, 0, -1.0f, "tpsstop");
    for (int i = 0; i < 2; ++i) {
        w.u16(static_cast<std::uint16_t>(i)).vec(i * 40.0f, 0, 0);
        w.u16(1).u32(i == 0 ? 1 : 0); // sinks
        w.u16(1).u32(i == 0 ? 0 : 1); // sources
        w.u32(0).u32(1).f32(0.5f).f32(0.6f);
    }
    w.u32(2);              // cells
    w.u32(0).u32(2).u32(0).u32(1); // near: cell 0 none, cell 1 both
    w.u32(0).u32(1).u32(1);        // in: cell 0 none, cell 1 path 1
    return w.d;
}

} // namespace

TEST(Mm1AiMap, ParsesPathsIntersectionsAndCellLists) {
    std::string err;
    auto m = parseMm1Bai(sampleBai(), &err);
    ASSERT_TRUE(m) << err;
    ASSERT_EQ(m->paths.size(), 2u);
    EXPECT_EQ(m->paths[0].stopLight, "tplttrafc");
    EXPECT_EQ(m->paths[0].oncoming, 1);
    EXPECT_TRUE(m->paths[0].divided);
    EXPECT_EQ(m->paths[0].sink, 1);
    ASSERT_EQ(m->paths[0].rows.size(), 2u);
    EXPECT_FLOAT_EQ(m->paths[0].rows[0][2].z, 2.5f);
    ASSERT_EQ(m->intersections.size(), 2u);
    EXPECT_EQ(m->intersections[1].position.x, 40.0f);
    EXPECT_EQ(m->cellPathsNear[1].size(), 2u);
    EXPECT_EQ(m->cellPathsIn[1][0], 1);

    auto truncated = sampleBai();
    truncated.pop_back();
    EXPECT_FALSE(parseMm1Bai(truncated, &err));
}

TEST(Mm1AiMap, PairsOncomingPathsIntoTwoSidedRoads) {
    auto m = parseMm1Bai(sampleBai());
    ASSERT_TRUE(m);
    const AiMap map = mm1ToAiMap(*m);
    ASSERT_EQ(map.paths.size(), 1u);
    const AiPath& p = map.paths[0];
    EXPECT_EQ(p.flags & 0x1, 0x1); // divided
    // The collapsed end points dropped: three sections left.
    EXPECT_EQ(p.center.size(), 3u);
    EXPECT_EQ(p.ends[0].intersection, 1u); // the first path's sink, at center.back()
    EXPECT_EQ(p.ends[1].intersection, 0u);
    EXPECT_EQ(p.ends[0].vehicleRule, 1u); // a traffic light
    EXPECT_EQ(p.ends[1].vehicleRule, 0u); // a stop sign
    // Right side: lane, sidewalk, curb, edge; left side the partner reversed
    // into the first path's order.
    ASSERT_EQ(p.right.polylines.size(), 4u);
    EXPECT_EQ(p.right.numLanes, 1);
    EXPECT_FLOAT_EQ(p.right.polylines[0][0].x, 10.0f);
    ASSERT_EQ(p.left.polylines.size(), 4u);
    EXPECT_FLOAT_EQ(p.left.polylines[0][0].x, 10.0f);
    EXPECT_FLOAT_EQ(p.left.polylines[0][0].z, -2.5f);
    EXPECT_NEAR(p.halfWidth, 5.0f, 1e-4f);
    ASSERT_EQ(map.intersections.size(), 2u);
    EXPECT_EQ(map.intersections[0].paths.size(), 1u);
    EXPECT_EQ(map.roomPathsNear[1].size(), 1u); // both MM1 paths are the one MM2 road
}
