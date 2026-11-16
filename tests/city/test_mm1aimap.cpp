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
void path(Writer& w, int id, int src, int sink, int oncoming, float dir, std::string_view light, int type) {
    const int vc = 5;
    w.u16(static_cast<std::uint16_t>(id)).u16(vc).u16(1).u16(1); // id, vertices, lanes, sidewalks
    w.u16(0).u16(static_cast<std::uint16_t>(type));              // stop light index, intersection type
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
    for (int k = 0; k < 4; ++k) // x axes, normals, z axes, sub-section directions (along the road)
        for (int i = 0; i < vc; ++i)
            w.vec(k == 3 ? dir : 0.0f, k == 1 ? 1.0f : 0.0f, k == 0 ? -dir : 0.0f);
    row(10.0f); // outer edge
    row(5.0f);  // curb
    row(-5.0f); // far curb
    // StopLightPos: a light's pole and axis point, none without one.
    if (type == 1)
        w.vec(35.0f, 0, 6.0f).vec(35.0f, 0, 7.0f);
    else
        w.vec(0, 0, 0).vec(0, 0, 0);
    for (int i = 0; i < 15; ++i)
        w.f32(0);
}

std::vector<std::byte> sampleBai() {
    Writer w;
    w.u16(2).u16(2);
    path(w, 0, 0, 1, 1, 1.0f, "tplttrafcdual", 1);
    path(w, 1, 1, 0, 0, -1.0f, "tplttrafc", 0); // a stop sign whatever its name
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
    EXPECT_EQ(m->paths[0].stopLight, "tplttrafcdual");
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
    // The collapsed end points dropped: three sections left, and a point
    // added in each of the two 10 m segments between them (MM1's rail
    // curves, 5 m apart at most).
    EXPECT_EQ(p.center.size(), 5u);
    EXPECT_FLOAT_EQ(p.center[1].x, 15.0f);
    EXPECT_EQ(p.ends[0].intersection, 1u); // the first path's sink, at center.back()
    EXPECT_EQ(p.ends[1].intersection, 0u);
    EXPECT_EQ(p.ends[0].vehicleRule, 1u); // a traffic light
    EXPECT_EQ(p.ends[1].vehicleRule, 0u); // a stop sign
    // The light's pole, axis point and model; none at the stop sign.
    EXPECT_FLOAT_EQ(p.ends[0].trafficLightPos.x, 35.0f);
    EXPECT_FLOAT_EQ(p.ends[0].trafficLightAxis.z, 7.0f);
    EXPECT_EQ(p.ends[0].trafficLightModel, "tplttrafcdual");
    EXPECT_TRUE(p.ends[1].trafficLightModel.empty());
    // Right side: lane, sidewalk, curb, edge; left side the partner reversed
    // into the first path's order.
    ASSERT_EQ(p.right.polylines.size(), 4u);
    EXPECT_EQ(p.right.numLanes, 1);
    EXPECT_FLOAT_EQ(p.right.polylines[0][0].x, 10.0f);
    ASSERT_EQ(p.left.polylines.size(), 4u);
    // The lane lines run in their side's own direction of travel, as MM2's
    // data has them (London, SF): the right side's with the centre line, the
    // left side's against it.
    EXPECT_FLOAT_EQ(p.left.polylines[0][0].x, 30.0f);
    EXPECT_FLOAT_EQ(p.left.polylines[0][0].z, -2.5f);
    EXPECT_GT(p.right.polylines[0].back().x, p.right.polylines[0].front().x);
    EXPECT_LT(p.left.polylines[0].back().x, p.left.polylines[0].front().x);
    // The sidewalk row of the left side runs with the section index again.
    EXPECT_GT(p.left.polylines[1].back().x, p.left.polylines[1].front().x);
    // z points back and w forward, the curve tangents (AiPath's frames).
    ASSERT_EQ(p.zAxis.size(), p.center.size());
    EXPECT_LT(p.zAxis[0].x, 0.0f);
    EXPECT_GT(p.wAxis[0].x, 0.0f);
    EXPECT_NEAR(p.halfWidth, 5.0f, 1e-4f);
    ASSERT_EQ(map.intersections.size(), 2u);
    EXPECT_EQ(map.intersections[0].paths.size(), 1u);
    EXPECT_EQ(map.roomPathsNear[1].size(), 1u); // both MM1 paths are the one MM2 road
}

TEST(Mm1AiMap, AdaptsMm1RaceSettings) {
    AiMapConfig cfg;
    cfg.speedLimit = 30.0f; // MM1: miles per hour
    cfg.opponents.push_back({"vpbug", "opp1r60.c24", {0.30f}});
    cfg.opponents.push_back({"vpbug", "opp8r60.c28", {0.95f}});
    cfg.opponents.push_back({"vpbug", "none", {}});
    adaptMm1AiMapConfig(cfg);
    EXPECT_NEAR(*cfg.speedLimit, 13.41f, 0.01f);
    // MM1's MaxThrottle stays as is (racers on MM1's physics drive at it).
    EXPECT_NEAR(cfg.opponents[0].params[0], 0.30f, 1e-6f);
    EXPECT_NEAR(cfg.opponents[1].params[0], 0.95f, 1e-6f);
    EXPECT_TRUE(cfg.opponents[2].params.empty()); // 1.0 (aiRaceData's default)
    // A racer left on MM2's physics gets it mapped onto MM2's range.
    EXPECT_NEAR(mm1MaxThrottleForMm2(0.30f), 0.85f, 1e-5f);
    EXPECT_NEAR(mm1MaxThrottleForMm2(0.95f), 0.98929f, 1e-4f); // still below a 1.0 car
}

TEST(Mm1AiMap, ClosesRoadsOverADrawbridgeLeaf) {
    // The sample's road runs along x from 0 to 40 (centre z = 0); a leaf
    // whose deck covers x 10..20 lies across it, another far away does not.
    auto m = parseMm1Bai(sampleBai());
    ASSERT_TRUE(m);
    Mm1LeafDeck over;
    over.frame = Mat34::translation({15.0f, 0.0f, 0.0f});
    over.box.expand({-5.0f, -1.0f, -5.0f});
    over.box.expand({5.0f, 0.2f, 5.0f});
    Mm1LeafDeck far = over;
    far.frame = Mat34::translation({15.0f, 0.0f, 500.0f});
    EXPECT_EQ(markMm1BridgePaths(*m, std::span<const Mm1LeafDeck>(&far, 1)), 0);
    EXPECT_EQ(markMm1BridgePaths(*m, std::span<const Mm1LeafDeck>(&over, 1)), 2);
    const AiMap map = mm1ToAiMap(*m);
    ASSERT_EQ(map.paths.size(), 1u);
    // Both sides closed to ambient traffic (1) and pedestrians (2).
    EXPECT_EQ(map.paths[0].right.roadType & 3, 3);
    EXPECT_EQ(map.paths[0].left.roadType & 3, 3);
}

TEST(Mm1AiMap, RailCurveIsHermite) {
    // aiRailSet's curve: through both ends with the given tangents; a
    // quarter turn of radius 10 bulges outwards from the chord.
    const Vec3 a{0, 0, 0}, b{10, 0, 10};
    const Vec3 ta{10, 0, 0}, tb{0, 0, 10};
    EXPECT_FLOAT_EQ(mm1RailCurve(a, b, ta, tb, 0.0f).x, 0.0f);
    EXPECT_FLOAT_EQ(mm1RailCurve(a, b, ta, tb, 1.0f).z, 10.0f);
    const Vec3 mid = mm1RailCurve(a, b, ta, tb, 0.5f);
    // h(0.5) = 0.5 a + 0.5 b + 0.125 ta - 0.125 tb.
    EXPECT_FLOAT_EQ(mid.x, 5.0f + 1.25f);
    EXPECT_FLOAT_EQ(mid.z, 5.0f - 1.25f);
    // y runs straight.
    EXPECT_FLOAT_EQ(mm1RailCurve({0, 2, 0}, {0, 4, 10}, ta, tb, 0.25f).y, 2.5f);
}

TEST(Mm1AiMap, DensifiesAlongTheCurveAndKeepsTheEnds) {
    auto m = parseMm1Bai(sampleBai());
    ASSERT_TRUE(m);
    const auto& path = m->paths[0];
    const auto splits = mm1CurveSplits(path, 5.0f);
    ASSERT_EQ(splits.size(), 4u);
    EXPECT_EQ(splits[0], 0); // to the intersection's centre: left alone
    EXPECT_EQ(splits[1], 1);
    EXPECT_EQ(splits[3], 0);
    const auto d = densifyMm1Path(path, splits);
    EXPECT_EQ(d.center.size(), 7u);
    EXPECT_EQ(d.vertexCount, 7);
    for (const auto& row : d.rows)
        EXPECT_EQ(row.size(), 7u);
    EXPECT_FLOAT_EQ(d.center.front().x, path.center.front().x);
    EXPECT_FLOAT_EQ(d.center.back().x, path.center.back().x);
}

// aiPath's small accessors (Mm1Rail.h; Open1560 game.asm).
#include "city/Mm1Rail.h"

TEST(Mm1Rail, PathAccessors) {
    Mm1AiPath p;
    p.vertexCount = 5;
    p.centerOffsets = {0, 10, 20, 35, 45};
    p.zAxis = {{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    p.subSectionDirs = {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}};
    EXPECT_FLOAT_EQ(mm1CenterLength(p, 1, 3), 25.0f);
    EXPECT_FLOAT_EQ(mm1CenterLength(p, 3, 1), -25.0f);
    // CenterIndex: from vertex 2 on, the first whose distance from vertex 1 reaches `dist`.
    EXPECT_EQ(mm1CenterIndex(p, -5.0f), 2);
    EXPECT_EQ(mm1CenterIndex(p, 9.0f), 2);
    EXPECT_EQ(mm1CenterIndex(p, 12.0f), 3);
    EXPECT_EQ(mm1CenterIndex(p, 100.0f), 3); // clamped to the length between vertices 1 and n-2
    const Vec3 d = mm1SubSectionDir(p, 1, 4.0f);
    EXPECT_FLOAT_EQ(d.z, 4.0f);
    // Entry: -ZAxis[n-2] (vertex 3), exit: -ZAxis[2], each times the scale.
    const Vec3 entry = mm1IntersectionEntryVector(p, 2.0f);
    EXPECT_FLOAT_EQ(entry.y, -2.0f);
    const Vec3 exit = mm1IntersectionExitVector(p, 3.0f);
    EXPECT_FLOAT_EQ(exit.x, -3.0f);
}
