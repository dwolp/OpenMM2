#include "game/SceneLights.h"

#include <gtest/gtest.h>

#include <vector>

using namespace mm2;

namespace {
render::SceneLight point(const Vec3& p, float range) {
    render::SceneLight l;
    l.position = p;
    l.range = range;
    return l;
}
} // namespace

TEST(SceneLights, PicksTheNearestAndSkipsThoseBehind) {
    std::vector<render::SceneLight> lights;
    lights.push_back(point({0, 5, -200}, 20));  // far ahead
    lights.push_back(point({0, 5, 100}, 20));   // far behind: dropped
    lights.push_back(point({0, 5, -10}, 20));   // near ahead
    lights.push_back(point({0, 5, 14}, 20));    // just behind, its reach in view: kept
    render::FrameConstants frame;
    game::selectSceneLights(lights, {0, 0, 0}, {0, 0, -1}, frame);
    ASSERT_EQ(frame.sceneLightCount, 3u);
    EXPECT_FLOAT_EQ(frame.sceneLights[0].position.z, -10.0f);
    EXPECT_FLOAT_EQ(frame.sceneLights[1].position.z, 14.0f);
    EXPECT_FLOAT_EQ(frame.sceneLights[2].position.z, -200.0f);
}

TEST(SceneLights, KeepsAtMostTheBudget) {
    std::vector<render::SceneLight> lights;
    for (int i = 0; i < 100; ++i)
        lights.push_back(point({0, 5, -static_cast<float>(i)}, 20));
    render::FrameConstants frame;
    game::selectSceneLights(lights, {0, 0, 0}, {0, 0, -1}, frame);
    ASSERT_EQ(frame.sceneLightCount, render::kMaxSceneLights);
    EXPECT_FLOAT_EQ(frame.sceneLights[0].position.z, 0.0f);
    EXPECT_FLOAT_EQ(frame.sceneLights[render::kMaxSceneLights - 1].position.z,
                    -static_cast<float>(render::kMaxSceneLights - 1));
}

TEST(SceneLights, LongBeamsDoNotCrowdOutNearLamps) {
    // A headlight 60 m away reaching 40 m against a lamp 25 m away reaching 24 m.
    std::vector<render::SceneLight> lights;
    lights.push_back(point({0, 1, -60}, 40));
    lights.push_back(point({0, 8, -25}, 24));
    render::FrameConstants frame;
    game::selectSceneLights(lights, {0, 0, 0}, {0, 0, -1}, frame);
    ASSERT_EQ(frame.sceneLightCount, 2u);
    EXPECT_FLOAT_EQ(frame.sceneLights[0].position.z, -25.0f);
}

TEST(SceneLights, CollectsOnlyWhileATargetIsSet) {
    std::vector<render::SceneLight> out;
    game::setSceneLightScales(2.0f, 1.0f);
    game::addSceneLight(game::SceneLightKind::Headlight, point({0, 0, 0}, 10)); // no target: dropped
    game::setSceneLightTarget(&out);
    game::addSceneLight(game::SceneLightKind::Headlight, point({0, 0, 0}, 10));
    game::setSceneLightScales(0.0f, 1.0f);
    game::addSceneLight(game::SceneLightKind::Headlight, point({0, 0, 0}, 10)); // strength 0: dropped
    game::setSceneLightTarget(nullptr);
    game::setSceneLightScales(1.0f, 1.0f);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].color.x, 2.0f);
}

// Past the budget, the kept lights fade out towards the first one left out,
// so a light dropped as the camera moves has already gone dark.
TEST(SceneLights, LightsNearTheBudgetCutFadeOut) {
    std::vector<render::SceneLight> lights;
    for (int i = 0; i < 60; ++i) {
        render::SceneLight l = point({0, 5, -2.0f * static_cast<float>(i)}, 1);
        l.color = {1, 1, 1};
        lights.push_back(l);
    }
    render::FrameConstants frame;
    game::selectSceneLights(lights, {0, 0, 0}, {0, 0, -1}, frame);
    ASSERT_EQ(frame.sceneLightCount, render::kMaxSceneLights);
    EXPECT_FLOAT_EQ(frame.sceneLights[0].color.x, 1.0f);
    EXPECT_LT(frame.sceneLights[render::kMaxSceneLights - 1].color.x, 0.05f);
    for (std::uint32_t i = 1; i < frame.sceneLightCount; ++i)
        EXPECT_LE(frame.sceneLights[i].color.x, frame.sceneLights[i - 1].color.x + 1e-6f);
}

// Street lamps fade in over the last 30% of the distance they are gathered from.
TEST(SceneLights, LampsFadeInAtTheEdgeOfTheirReach) {
    const Vec3 eye{0, 0, 0};
    const float full = game::lampLight({0, 8, -50}).color.x;
    EXPECT_FLOAT_EQ(game::lampLight({0, 8, -50}, eye, 140.0f).color.x, full);
    const float edge = game::lampLight({0, 8, -125}, eye, 140.0f).color.x;
    EXPECT_GT(edge, 0.0f);
    EXPECT_LT(edge, full * 0.5f);
    EXPECT_NEAR(game::lampLight({0, 8, -139.9f}, eye, 140.0f).color.x, 0.0f, 1e-3f);
}
