#include "game/SunShadow.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace mm2;
using namespace mm2::game;

namespace {

// Where a world point lands in the shadow map: (x, y) in -1..1, z in 0..1.
Vec3 toMap(const SunShadowView& v, const Vec3& p) {
    const Mat44 m = v.viewProj();
    const float x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const float y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const float z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    return {x, y, z};
}

Mat34 cameraAt(const Vec3& p) {
    Mat34 m; // looking down -Z
    m.m3 = p;
    return m;
}

} // namespace

TEST(SunShadow, CoversTheGroundAroundTheCamera) {
    const Vec3 sun{-0.22f, -0.84f, -0.49f}; // London's noon key light
    const SunShadowSettings s;
    const SunShadowView v = sunShadowView(sun, cameraAt({400.0f, 5.0f, -200.0f}), s);
    for (const Vec3& p : {Vec3{400.0f, 0.0f, -200.0f}, Vec3{400.0f, 0.0f, -240.0f}, Vec3{380.0f, 0.0f, -210.0f},
                          Vec3{400.0f, 40.0f, -220.0f}}) {
        const Vec3 q = toMap(v, p);
        EXPECT_LT(std::abs(q.x), 1.0f);
        EXPECT_LT(std::abs(q.y), 1.0f);
        EXPECT_GT(q.z, 0.0f);
        EXPECT_LT(q.z, 1.0f);
    }
    EXPECT_FLOAT_EQ(v.texel, 2.0f * s.radius / static_cast<float>(s.size));
}

// The square snaps to whole texels: as the camera creeps forward a fixed
// world point moves across the map in whole texels or not at all, so
// shadow edges do not shimmer.
TEST(SunShadow, SnapsToWholeTexels) {
    const Vec3 sun{-0.5f, -0.7f, 0.3f};
    const SunShadowSettings s;
    const Vec3 world{10.0f, 0.0f, 3.0f};
    const SunShadowView a = sunShadowView(sun, cameraAt({0.0f, 2.0f, 0.0f}), s);
    for (float dz : {0.013f, 0.27f, 1.9f}) {
        const SunShadowView b = sunShadowView(sun, cameraAt({0.0f, 2.0f, -dz}), s);
        const Vec3 qa = toMap(a, world), qb = toMap(b, world);
        const float texelsX = (qb.x - qa.x) * 0.5f * static_cast<float>(s.size);
        const float texelsY = (qb.y - qa.y) * 0.5f * static_cast<float>(s.size);
        EXPECT_NEAR(texelsX, std::round(texelsX), 0.02f) << dz;
        EXPECT_NEAR(texelsY, std::round(texelsY), 0.02f) << dz;
    }
}

TEST(SunShadow, DarknessFollowsTheSun) {
    render::FrameConstants f;
    f.ambient = {0.25f, 0.25f, 0.25f};
    f.lights[0] = {Vec3{-0.3f, -0.8f, -0.5f}, Vec3{1.0f, 1.0f, 1.0f}};
    const float noon = sunShadowDarkness(f);
    EXPECT_GT(noon, 0.5f);
    EXPECT_LE(noon, 1.0f);
    f.lights[0].color = {0.2f, 0.2f, 0.25f}; // a dim key light (night)
    EXPECT_LT(sunShadowDarkness(f), noon);
    f.lights[0] = {Vec3{0.0f, 0.5f, -0.8f}, Vec3{1.0f, 1.0f, 1.0f}}; // below the horizon
    EXPECT_EQ(sunShadowDarkness(f), 0.0f);
}

// The cascades grow outwards from the camera, each covering the last, and
// their cells tile the map.
// The far cascades snap to a sixteenth of their width: a short drive moves
// the near ones but not them.
TEST(SunShadow, FarCascadesStayPutForAWhile) {
    const Vec3 sun{-0.5f, -0.4f, -0.77f};
    SunShadowCascadeSettings s;
    s.count = 4;
    const SunShadowCascades a = sunShadowCascades(sun, cameraAt({100.0f, 3.0f, 100.0f}), s);
    int nearMoved = 0, farMoved = 0;
    for (float dz = 1.0f; dz <= 20.0f; dz += 1.0f) {
        const SunShadowCascades b = sunShadowCascades(sun, cameraAt({100.0f, 3.0f, 100.0f - dz}), s);
        nearMoved += b.views[0].eye.dist2(a.views[0].eye) > 0.0f ? 1 : 0;
        farMoved += b.views[3].eye.dist2(a.views[3].eye) > 0.0f ? 1 : 0;
    }
    EXPECT_GT(nearMoved, 15);
    EXPECT_LE(farMoved, 1);
}

TEST(SunShadow, CascadesDoNotMoveAsTheCameraTurns) {
    const Vec3 sun{-0.5f, -0.2f, -0.84f}; // low: long shadows
    const SunShadowCascadeSettings s;
    const Vec3 at{30.0f, 4.0f, -55.0f};
    const SunShadowCascades a = sunShadowCascades(sun, cameraAt(at), s);
    for (const float yaw : {0.7f, 1.9f, 3.1f}) {
        Mat34 turned = cameraAt(at);
        turned.m0 = {std::cos(yaw), 0.0f, -std::sin(yaw)};
        turned.m2 = {std::sin(yaw), 0.0f, std::cos(yaw)};
        const SunShadowCascades b = sunShadowCascades(sun, turned, s);
        ASSERT_EQ(a.count, b.count);
        for (std::uint32_t i = 0; i < a.count; ++i) {
            const Mat44 ma = a.views[i].viewProj(), mb = b.views[i].viewProj();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    EXPECT_FLOAT_EQ(ma.m[r][c], mb.m[r][c]) << yaw << " cascade " << i;
        }
    }
}

TEST(SunShadow, CascadesNestAndTileTheMap) {
    const Vec3 sun{-0.5f, -0.4f, -0.77f}; // a low morning sun
    SunShadowCascadeSettings s;
    s.count = 4;
    s.cellSize = 1024;
    s.radius = 80.0f;
    const SunShadowCascades c = sunShadowCascades(sun, cameraAt({10.0f, 5.0f, 20.0f}), s);
    ASSERT_EQ(c.count, 4u);
    for (std::uint32_t i = 1; i < c.count; ++i) {
        EXPECT_NEAR(c.views[i].texel, c.views[i - 1].texel * s.growth, 1e-4f);
        EXPECT_GE(c.views[i].depth, c.views[i - 1].depth);
    }
    EXPECT_GE(c.views[3].depth, 2.0f * 80.0f * 27.0f);
    // A point near the camera is inside every cascade; one 1 km away only in the last.
    for (std::uint32_t i = 0; i < c.count; ++i) {
        const Vec3 q = toMap(c.views[i], {10.0f, 0.0f, 0.0f});
        EXPECT_LT(std::abs(q.x), 1.0f) << i;
        EXPECT_LT(std::abs(q.y), 1.0f) << i;
    }
    const Vec3 far = toMap(c.views[3], {10.0f, 0.0f, -1000.0f});
    EXPECT_LT(std::abs(far.x), 1.0f);
    EXPECT_LT(std::abs(far.y), 1.0f);
    const Vec3 near = toMap(c.views[0], {10.0f, 0.0f, -1000.0f});
    EXPECT_GT(std::max(std::abs(near.x), std::abs(near.y)), 1.0f);
    bool used[2][2] = {};
    for (std::size_t i = 0; i < 4; ++i) {
        const render::Rect cell = sunShadowCell(i, 1024);
        EXPECT_EQ(cell.width, 1024u);
        used[cell.x / 1024][cell.y / 1024] = true;
    }
    EXPECT_TRUE(used[0][0] && used[0][1] && used[1][0] && used[1][1]);
}
