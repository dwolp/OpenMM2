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
