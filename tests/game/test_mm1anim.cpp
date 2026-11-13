// MM1's El train and plane (mmAnimTrain, mmAnimPlane) in game::world::Mm1Anim.
#include "game/world/Mm1Anim.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace mm2;
using namespace mm2::game::world;

namespace {

// A 100 m square loop at 12 m, its first point repeated as Chicago's is.
std::vector<Vec3> squareLoop() {
    return {{0, 12, 0}, {0, 12, 0}, {100, 12, 0}, {100, 12, 100}, {0, 12, 100}, {0, 12, 0}};
}

} // namespace

TEST(Mm1Anim, ParsesPointLists) {
    const auto p = parseMm1AnimPath("162.327,12.0004,-188.787\r\n162.336,12.0003,-159.618\n\nbad\n");
    ASSERT_EQ(p.size(), 2u);
    EXPECT_FLOAT_EQ(p[0].x, 162.327f);
    EXPECT_FLOAT_EQ(p[1].z, -159.618f);
}

TEST(Mm1Anim, TrainCurvePassesThroughThePoints) {
    const auto pts = squareLoop();
    std::vector<float> len;
    for (std::size_t i = 0; i < pts.size(); ++i)
        len.push_back(pts[(i + 1) % pts.size()].dist(pts[i]));
    const auto c = mm1TrainCubic(pts, len, 2); // (100,0) -> (100,100)
    EXPECT_NEAR(c.position(0.0f).x, 100.0f, 1e-4f);
    EXPECT_NEAR(c.position(1.0f).z, 100.0f, 1e-4f);
    EXPECT_FLOAT_EQ(c.position(0.5f).y, 12.0f);
    // The tangent at the start: unit(p[3] - p[1]) times the length.
    const Vec3 d = c.direction(0.0f);
    EXPECT_NEAR(d.x, 100.0f / std::sqrt(2.0f), 1e-3f);
    EXPECT_NEAR(d.z, 100.0f / std::sqrt(2.0f), 1e-3f);
}

TEST(Mm1Anim, PlaneCurveIsABSpline) {
    // On a straight line of evenly spaced points a B-spline stays on the
    // line, at the point's position at t = 0.
    std::vector<Vec3> pts;
    for (int i = 0; i < 8; ++i)
        pts.push_back({static_cast<float>(i) * 10.0f, 0, 0});
    const auto c = mm1PlaneCubic(pts, 3);
    EXPECT_NEAR(c.position(0.0f).x, 30.0f, 1e-4f);
    EXPECT_NEAR(c.position(0.5f).x, 35.0f, 1e-4f);
    EXPECT_NEAR(c.direction(0.5f).x, 10.0f, 1e-4f);
}

TEST(Mm1Anim, FrameFacesAlongTheDirection) {
    const Mat34 m = mm1AnimFrame({1, 2, 3}, {10, 0, 0});
    // Angel objects face -z: -z is +x here.
    EXPECT_NEAR(m.m2.x, -1.0f, 1e-6f);
    EXPECT_NEAR(m.m1.y, 1.0f, 1e-6f);
    EXPECT_NEAR(m.m0.z, 1.0f, 1e-6f); // Y x z
    EXPECT_FLOAT_EQ(m.m3.z, 3.0f);
}

TEST(Mm1Anim, TrainCarsFollowAndLapInSixtyFiveSeconds) {
    auto t = Mm1Anim::makeTrain(squareLoop(), "r_l_train", 3, 15.0f);
    EXPECT_FLOAT_EQ(t.total, 400.0f);
    EXPECT_FLOAT_EQ(t.speed, 400.0f / 65.0f);
    ASSERT_EQ(t.cars.size(), 3u);
    // The first car at the start, the others 15 m and 30 m behind it (from
    // the end of the loop back).
    EXPECT_EQ(t.cars[0].startSegment, 1);
    EXPECT_FLOAT_EQ(t.cars[0].startT, 0.0f);
    EXPECT_EQ(t.cars[1].startSegment, 4);
    EXPECT_NEAR(t.cars[1].startT, 0.85f, 1e-5f);
    for (auto& c : t.cars) {
        c.segment = c.startSegment;
        c.t = c.startT;
    }
    // 65 s later (in 0.1 s steps) the first car is back near its start;
    // empty segments are skipped.
    for (int i = 0; i < 650; ++i)
        Mm1Anim::updateTrain(t, 0.1f);
    const Vec3 at = t.cars[0].matrix.m3;
    EXPECT_NEAR(at.x, 0.0f, 1.0f);
    EXPECT_NEAR(at.z, 0.0f, 1.0f);
    for (const auto& c : t.cars)
        EXPECT_NE(c.segment, 0); // the empty first segment
}

TEST(Mm1Anim, PlaneLapsInSixtySecondsTenMetresUp) {
    std::vector<Vec3> pts{{0, 0, 0}, {100, 0, 0}, {100, 0, 100}, {0, 0, 100}, {0, 0, 0}};
    auto p = Mm1Anim::makePlane(pts, "vaboeing");
    EXPECT_FLOAT_EQ(p.total, 400.0f);
    EXPECT_FLOAT_EQ(p.speed, 400.0f / 60.0f);
    Mm1Anim::updatePlane(p, 0.0f);
    EXPECT_NEAR(p.matrix.m3.y, 10.0f, 1e-4f);
    for (int i = 0; i < 310; ++i)
        Mm1Anim::updatePlane(p, 0.1f);
    EXPECT_EQ(p.segment, 2); // just past half way round (31 s of 60)
}
