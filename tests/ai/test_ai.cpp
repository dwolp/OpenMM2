#include "TestData.h"
#include "ai/World.h"
#include "city/CityData.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <map>

using namespace mm2;

namespace {

// A straight two-way road along -Z from z=0 to z=-length, one lane each side
// at +-2 m (left lanes stored reversed, as in the retail files), sidewalks at
// +-6.5 m between curbs at +-5 and edges at +-8.
city::AiMap straightRoad(float length = 100.0f) {
    city::AiMap map;
    city::AiPath p;
    p.id = 0;
    p.halfWidth = 8.0f;
    p.speedLimit = 15.0f;
    const int sections = 3;
    for (int i = 0; i < sections; ++i) {
        const float z = -length * static_cast<float>(i) / (sections - 1);
        p.center.push_back({0, 0, z});
        // z axis points back (+Z, against increasing index), x to the left of travel (-X).
        p.xAxis.push_back({-1, 0, 0});
        p.yAxis.push_back({0, 1, 0});
        p.zAxis.push_back({0, 0, 1});
        p.wAxis.push_back({0, 0, -1});
        if (i > 0)
            p.centerLengths.push_back(length * static_cast<float>(i) / (sections - 1));
    }
    auto side = [&](float sign, bool reversed) {
        city::AiRoadSide s;
        s.numLanes = 1;
        s.numSidewalks = 1;
        std::vector<Vec3> lane, walk, curb, edge;
        for (int i = 0; i < sections; ++i) {
            const int k = reversed ? sections - 1 - i : i;
            lane.push_back(p.center[static_cast<std::size_t>(k)] + Vec3{-2.0f * sign, 0, 0});
        }
        for (int i = 0; i < sections; ++i) {
            walk.push_back(p.center[static_cast<std::size_t>(i)] + Vec3{-6.5f * sign, 0.07f, 0});
            curb.push_back(p.center[static_cast<std::size_t>(i)] + Vec3{-5.0f * sign, 0, 0});
            edge.push_back(p.center[static_cast<std::size_t>(i)] + Vec3{-8.0f * sign, 0.15f, 0});
        }
        s.polylines = {lane, walk, curb, edge};
        return s;
    };
    p.left = side(1.0f, true); // x axis is -X, so "left" lanes are at -X
    p.right = side(-1.0f, false);
    p.ends[0].intersection = 0; // at center.back()
    p.ends[1].intersection = 1; // at center.front()
    p.ends[0].vehicleRule = 1;
    p.ends[1].vehicleRule = 3;
    p.ends[0].trafficLightPos = {-7, 0, -length};
    p.ends[0].trafficLightAxis = {-6, 0, -length};
    map.paths.push_back(p);
    map.intersections.push_back({0, 0, {0, 0, -length}, {0}});
    map.intersections.push_back({1, 0, {0, 0, 0}, {0}});
    return map;
}

const city::CityData* retailCity(std::string_view name) {
    static std::map<std::string, std::unique_ptr<city::CityData>> cache;
    const std::string key(name);
    if (auto it = cache.find(key); it != cache.end())
        return it->second.get();
    auto c = city::loadCity(*test::gameData(), name);
    auto& slot = cache[key] = c ? std::make_unique<city::CityData>(std::move(*c)) : nullptr;
    return slot.get();
}

} // namespace

TEST(AiPolyline, PointAtAndProject) {
    ai::Polyline p;
    p.points = {{0, 0, 0}, {0, 0, -10}, {10, 0, -10}};
    p.finalize();
    EXPECT_FLOAT_EQ(p.length, 20.0f);
    Vec3 dir;
    const Vec3 a = p.pointAt(15.0f, &dir);
    EXPECT_NEAR(a.x, 5.0f, 1e-5f);
    EXPECT_NEAR(dir.x, 1.0f, 1e-5f);
    float d = 0.0f;
    EXPECT_NEAR(p.project({3, 0, -5}, &d), 5.0f, 1e-4f);
    EXPECT_NEAR(d, 3.0f, 1e-4f);
    EXPECT_NEAR(p.pointAt(99.0f).x, 10.0f, 1e-5f); // clamped
}

// MM1 roads keep their own limit (aiGoalRandomDrive::SolveVelocity adds only
// the car's excess); MM2's take the AI map's [Speed Limit], plus 12.5 on a
// freeway (inferred for MM1: nothing in its code reads the race file's limit).
TEST(AiRoadNetwork, Mm1RoadsKeepTheirOwnSpeedLimit) {
    auto map = straightRoad();
    map.paths[0].speedLimit = 28.0f;
    map.paths[0].flags = 0x4;
    ai::NetworkOptions options;
    options.defaultSpeedLimit = 13.4f;
    EXPECT_FLOAT_EQ(ai::RoadNetwork::build(map, options).paths()[0].speedLimit, 13.4f + 12.5f);
    map.mm1 = true;
    EXPECT_FLOAT_EQ(ai::RoadNetwork::build(map, options).paths()[0].speedLimit, 28.0f);
}

TEST(AiRoadNetwork, LaneDirectionsAndDriveOnLeft) {
    const auto map = straightRoad();
    const auto right = ai::RoadNetwork::build(map, {});
    ASSERT_EQ(right.lanes().size(), 2u);
    // Right side lane runs with increasing section index (towards -Z) and
    // keeps to the right of travel (+X).
    const auto& r = right.lanes()[1];
    EXPECT_EQ(r.side, 1);
    EXPECT_GT(r.line.points.front().z, r.line.points.back().z);
    EXPECT_NEAR(r.line.points.front().x, 2.0f, 1e-5f);
    EXPECT_EQ(r.toIntersection, 0);
    EXPECT_EQ(r.rule, ai::EntryRule::TrafficLight);
    EXPECT_GE(r.lightSlot, 0);
    // Left lane runs back towards +Z, on the other side.
    const auto& l = right.lanes()[0];
    EXPECT_LT(l.line.points.front().z, l.line.points.back().z);
    EXPECT_NEAR(l.line.points.front().x, -2.0f, 1e-5f);
    EXPECT_EQ(l.toIntersection, 1);
    EXPECT_EQ(right.sidewalks().size(), 2u);
    EXPECT_NEAR(right.sidewalks()[0].halfWidth, 1.5f, 1e-4f);
    EXPECT_NEAR(right.sidewalks()[0].centre.points.front().y, 0.15f, 1e-5f); // sidewalk surface

    ai::NetworkOptions opts;
    opts.driveOnLeft = true;
    const auto left = ai::RoadNetwork::build(map, opts);
    // Same directions, each on the other side's lane line (aiPath::ReverseDirection).
    EXPECT_NEAR(left.lanes()[1].line.points.front().x, -2.0f, 1e-5f);
    EXPECT_GT(left.lanes()[1].line.points.front().z, left.lanes()[1].line.points.back().z);
    EXPECT_NEAR(left.lanes()[0].line.points.front().x, 2.0f, 1e-5f);
}

namespace {

// Intersection 0 with three approaches (copies of the straight road), each
// with a light at its ends[0].
city::AiMap threeWay() {
    city::AiMap map = straightRoad();
    auto second = map.paths[0];
    second.id = 1;
    auto third = map.paths[0];
    third.id = 2;
    map.paths.push_back(second);
    map.paths.push_back(third);
    map.intersections[0].paths = {0, 1, 2};
    map.intersections[1].paths = {0, 1, 2};
    return map;
}

// threeWay() plus an uncontrolled road whose direction -1 lanes arrive at
// intersection 0 through its ends[1]: a source without a light.
city::AiMap threeWayPlusUncontrolled() {
    city::AiMap map = threeWay();
    auto fourth = map.paths[0];
    fourth.id = 3;
    std::swap(fourth.ends[0], fourth.ends[1]);
    fourth.ends[0].vehicleRule = 3;
    fourth.ends[1].vehicleRule = 3;
    map.paths.push_back(fourth);
    map.intersections[0].paths.push_back(3);
    map.intersections[1].paths.push_back(3);
    return map;
}

struct LightClock {
    ai::TrafficLights& lights;
    float t = 0.0f;
    void run(float until, float dt = 0.1f) {
        while (t + dt * 0.5f < until) {
            lights.update(dt);
            t += dt;
        }
    }
};

} // namespace

TEST(AiTrafficLights, CycleMatchesMM2) {
    // Not every source has a light: plain rotation (aiTrafficLightSet mode 0).
    const auto map = threeWayPlusUncontrolled();
    const auto net = ai::RoadNetwork::build(map, {});
    ASSERT_EQ(net.lights().size(), 3u);
    EXPECT_EQ(net.intersections()[0].cycle, ai::LightCycle::Rotate);
    ai::TrafficLights lights;
    lights.build(net);
    EXPECT_EQ(lights.state(0), ai::LightState::Green);
    EXPECT_EQ(lights.state(1), ai::LightState::Red);
    LightClock clock{lights};
    clock.run(2.9f);
    EXPECT_EQ(lights.state(0), ai::LightState::Green);
    clock.run(3.2f); // amber for the last 3 s of the 6 s cycle
    EXPECT_EQ(lights.state(0), ai::LightState::Amber);
    clock.run(6.2f);
    EXPECT_EQ(lights.state(0), ai::LightState::Red);
    EXPECT_EQ(lights.state(1), ai::LightState::Green);
    EXPECT_EQ(lights.state(2), ai::LightState::Red);
    ASSERT_EQ(lights.newGreens().size(), 0u); // reported on the step that switched only
    clock.run(18.4f);
    EXPECT_EQ(lights.state(0), ai::LightState::Green); // wrapped round
}

TEST(AiTrafficLights, WalkPhaseWhenEveryApproachHasALight) {
    // Every road arriving at intersection 0 has a light there.
    const auto net = ai::RoadNetwork::build(threeWay(), {});
    EXPECT_EQ(net.intersections()[0].cycle, ai::LightCycle::AllSources);
    ai::TrafficLights lights;
    lights.build(net);
    LightClock clock{lights};
    clock.run(6.2f);
    EXPECT_EQ(lights.state(1), ai::LightState::Green);
    // Each light lasts 61 steps of 0.1 s (the timer restarts at 0 once it
    // exceeds 6 s): after three, all red with WALK.
    clock.run(18.5f);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(lights.state(i), ai::LightState::Walk);
    clock.run(21.6f); // the last 3 s: don't walk
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(lights.state(i), ai::LightState::WalkEnd);
    clock.run(24.5f); // a new round
    EXPECT_EQ(lights.state(0), ai::LightState::Green);
    EXPECT_EQ(lights.state(1), ai::LightState::Red);
}

TEST(AiWorld, RetailCitiesBuild) {
    MM2_REQUIRE_GAME_DATA();
    for (const char* name : {"london", "sf"}) {
        const auto* c = retailCity(name);
        ASSERT_TRUE(c) << name;
        std::string err;
        auto world = ai::World::create(*c, *test::gameData(), {}, nullptr, &err);
        ASSERT_TRUE(world) << err;
        EXPECT_GT(world->network().lanes().size(), 500u);
        EXPECT_GT(world->network().sidewalks().size(), 400u);
        EXPECT_GT(world->network().lights().size(), 300u);
        EXPECT_EQ(world->network().driveOnLeft(), std::string_view(name) == "london");
        EXPECT_EQ(world->pedestrians().types().size(), 4u);
        for (const auto& s : world->signals())
            EXPECT_TRUE(test::gameData()->exists("geometry/" + s.model + ".pkg")) << s.model;
    }
}

TEST(AiWorld, DeterministicAndWellBehaved) {
    MM2_REQUIRE_GAME_DATA();
    const auto* c = retailCity("london");
    ASSERT_TRUE(c);
    ai::Settings settings;
    settings.seed = 7;
    auto a = ai::World::create(*c, *test::gameData(), settings);
    auto b = ai::World::create(*c, *test::gameData(), settings);
    ASSERT_TRUE(a && b);
    // Player waiting near the intersection with the most lights.
    const auto& net = a->network();
    std::size_t busiest = 0;
    for (std::size_t i = 0; i < net.intersections().size(); ++i)
        if (net.intersections()[i].lights.size() > net.intersections()[busiest].lights.size())
            busiest = i;
    const Vec3 player = net.intersections()[busiest].centre;
    float maxLaneError = 0.0f;
    int redRuns = 0;
    std::vector<float> redFor(a->signals().size(), 0.0f);
    std::map<int, bool> wasTurning;
    std::map<int, bool> wasEntered;
    std::map<int, int> committedBeforeRed; // the lane a car committed on before red, else -1
    for (int i = 0; i < 30 * 120; ++i) {
        a->step(player, {});
        b->step(player, {});
        for (const auto& car : a->cars()) {
            const auto d = a->traffic().debug(car.id);
            if (!d.turning && !d.changingLane && d.goal == ai::AmbientGoal::RandomDrive && d.lane >= 0) {
                float dist = 0.0f;
                net.lanes()[static_cast<std::size_t>(d.lane)].line.project(car.transform.m3, &dist);
                maxLaneError = std::max(maxLaneError, dist);
            }
            // A car that committed to the intersection (aiVehicleSpline's
            // enterInt) before its light turned red goes on whatever the
            // light does next, however long the queue ahead keeps it.
            if (d.entered && !wasEntered[car.id] && d.lane >= 0) {
                const auto& lane = net.lanes()[static_cast<std::size_t>(d.lane)];
                const bool red =
                    lane.lightSlot >= 0 && redFor[static_cast<std::size_t>(lane.lightSlot)] > 0.0f;
                committedBeforeRed[car.id] = red ? -1 : d.lane;
            }
            wasEntered[car.id] = d.entered;
            if (d.turning && !wasTurning[car.id] && d.lane >= 0) {
                const auto& lane = net.lanes()[static_cast<std::size_t>(d.lane)];
                const auto it = committedBeforeRed.find(car.id);
                const bool committed = it != committedBeforeRed.end() && it->second == d.lane;
                if (lane.lightSlot >= 0 && redFor[static_cast<std::size_t>(lane.lightSlot)] > 2.0f &&
                    !committed)
                    ++redRuns;
            }
            wasTurning[car.id] = d.turning;
        }
        for (std::size_t k = 0; k < redFor.size(); ++k) {
            const auto state = a->signals()[k].state;
            redFor[k] = state != ai::LightState::Green && state != ai::LightState::Amber
                            ? redFor[k] + ai::kAiStepSeconds
                            : 0.0f;
        }
    }
    // Same inputs, same seed: bit-identical state.
    ASSERT_EQ(a->cars().size(), b->cars().size());
    for (std::size_t i = 0; i < a->cars().size(); ++i) {
        EXPECT_EQ(a->cars()[i].transform.m3, b->cars()[i].transform.m3);
        EXPECT_EQ(a->cars()[i].speed, b->cars()[i].speed);
    }
    ASSERT_EQ(a->peds().size(), b->peds().size());
    for (std::size_t i = 0; i < a->peds().size(); ++i)
        EXPECT_EQ(a->peds()[i].transform.m3, b->peds()[i].transform.m3);
    EXPECT_GT(a->cars().size(), 10u);
    // Lane randomness is +-0.5 m; the Hermite sections bow a little off the
    // polyline between the vertices.
    std::printf("max lane deviation %.2f m, %zu cars\n", maxLaneError, a->cars().size());
    EXPECT_LE(maxLaneError, 1.5f);
    EXPECT_EQ(redRuns, 0);
}

TEST(AiWorld, PedestriansWalkSidewalksAndDive) {
    MM2_REQUIRE_GAME_DATA();
    const auto* c = retailCity("sf");
    ASSERT_TRUE(c);
    ai::Settings settings;
    settings.trafficDensity = 0.0f; // only pedestrians
    auto world = ai::World::create(*c, *test::gameData(), settings);
    ASSERT_TRUE(world);
    const auto& net = world->network();
    // Stand still near some sidewalks so pedestrians appear around us.
    const Vec3 home = net.sidewalks()[net.sidewalks().size() / 2].centre.pointAt(10.0f) + Vec3{40, 0, 0};
    for (int i = 0; i < 30 * 20; ++i)
        world->step(home, {});
    ASSERT_FALSE(world->peds().empty());
    // Walking along a sidewalk (not across a road), within the lateral
    // spread of MM2's curves (1.5 m) plus the steering lag. Not all of them:
    // a pedestrian turned back onto its previous road at a corner by an
    // obstacle (aiPedestrian::AvoidObstacle) keeps its old direction as the
    // previous one and so walks the corner line towards that road's far end,
    // as in MM2.
    int walking = 0, onSidewalk = 0;
    for (const auto& p : world->peds()) {
        if (p.state != "WALK" || p.crossing || p.sidewalk < 0)
            continue;
        ++walking;
        if (world->pedestrians().distanceFromSidewalk(p.id) <= 3.0f)
            ++onSidewalk;
    }
    EXPECT_GE(onSidewalk * 10, walking * 9) << onSidewalk << " of " << walking;
    // Drive straight at a walking pedestrian along its walking line.
    const ai::Pedestrian* target = nullptr;
    for (const auto& p : world->peds())
        if (p.state == "WALK")
            target = &p;
    ASSERT_TRUE(target);
    const int id = target->id;
    const Vec3 fwd = -target->transform.m2;
    Vec3 car = target->transform.m3 + fwd * 30.0f; // ahead of it, coming towards it
    const Vec3 vel = -fwd * 20.0f;
    bool dived = false;
    for (int i = 0; i < 30 * 3 && !dived; ++i) {
        car += vel * ai::kAiStepSeconds;
        world->step(car, vel);
        for (const auto& p : world->peds())
            if (p.id == id && p.state.find("DIVE") != std::string::npos)
                dived = true;
    }
    EXPECT_TRUE(dived);
}
