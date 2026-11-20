// Ambient traffic on synthetic road networks: MM2's route choice, lane
// order, stopping at lights, spawning density and lane changes.
#include "ai/AmbientRoute.h"
#include "ai/Traffic.h"
#include "ai/VehicleData.h"
#include "ai/World.h"

#include <gtest/gtest.h>

#include <map>
#include <set>

using namespace mm2;

namespace {

// A straight two-way road from `a` (ends[1], center.front()) to `b`
// (ends[0], center.back()) with `lanes` lanes per side 4 m apart (lane 0 at
// 2 m), sidewalks at 3.5 m beyond the outer lane between a curb and an edge,
// laid out as the retail files do: the first side's lanes stored in their
// own travel order (towards a), the second side's towards b.
city::AiPath road(int id, Vec3 a, Vec3 b, std::uint32_t nodeA, std::uint32_t nodeB, int lanes = 1,
                  int sections = 4) {
    city::AiPath p;
    p.id = static_cast<std::uint16_t>(id);
    p.flags = 0x8; // flat
    const float outer = 4.0f * static_cast<float>(lanes);
    p.halfWidth = outer + 3.0f;
    const Vec3 d = (b - a).normalized();
    const Vec3 left{d.z, 0.0f, -d.x};
    for (int i = 0; i < sections; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sections - 1);
        p.center.push_back(lerp(a, b, t));
        p.xAxis.push_back(left);
        p.yAxis.push_back(Vec3::yAxis());
        p.zAxis.push_back(-d);
        p.wAxis.push_back(d);
        if (i > 0)
            p.centerLengths.push_back(a.dist(b) * t);
    }
    auto side = [&](float sign, bool reversed) {
        city::AiRoadSide s;
        s.numLanes = static_cast<std::uint16_t>(lanes);
        s.numSidewalks = 1;
        for (int l = 0; l < lanes; ++l) {
            std::vector<Vec3> lane;
            for (int i = 0; i < sections; ++i) {
                const int k = reversed ? sections - 1 - i : i;
                const float offset = 2.0f + 4.0f * static_cast<float>(l);
                lane.push_back(p.center[static_cast<std::size_t>(k)] + left * (sign * offset));
            }
            s.polylines.push_back(lane);
        }
        std::vector<Vec3> walk, curb, edge;
        for (int i = 0; i < sections; ++i) {
            walk.push_back(p.center[static_cast<std::size_t>(i)] + left * (sign * (outer + 1.5f)));
            curb.push_back(p.center[static_cast<std::size_t>(i)] + left * (sign * outer));
            edge.push_back(p.center[static_cast<std::size_t>(i)] + left * (sign * (outer + 3.0f)));
        }
        s.polylines.push_back(walk);
        s.polylines.push_back(curb);
        s.polylines.push_back(edge);
        return s;
    };
    p.left = side(1.0f, true);
    p.right = side(-1.0f, false);
    p.ends[0].intersection = nodeB;
    p.ends[1].intersection = nodeA;
    p.ends[0].vehicleRule = 3;
    p.ends[1].vehicleRule = 3;
    return p;
}

void linkIntersections(city::AiMap& map) {
    for (auto& node : map.intersections)
        for (std::size_t k = 0; k < node.paths.size(); ++k) {
            auto& p = map.paths[node.paths[k]];
            for (auto& e : p.ends)
                if (e.intersection == node.id)
                    e.roadIndex = static_cast<std::uint16_t>(k);
        }
}

// A square block, 200 m a side: road k runs from corner k to corner k + 1.
// Corner 1 has traffic lights on both of its roads.
city::AiMap square() {
    city::AiMap map;
    const Vec3 corner[4] = {{0, 0, 0}, {0, 0, -200}, {200, 0, -200}, {200, 0, 0}};
    for (int k = 0; k < 4; ++k)
        map.paths.push_back(road(k, corner[k], corner[(k + 1) % 4], static_cast<std::uint32_t>(k),
                                 static_cast<std::uint32_t>((k + 1) % 4)));
    for (int k = 0; k < 4; ++k)
        map.intersections.push_back(
            {static_cast<std::uint16_t>(k), 0, corner[k],
             {static_cast<std::uint32_t>((k + 3) % 4), static_cast<std::uint32_t>(k)}});
    map.paths[0].ends[0].vehicleRule = 1; // road 0 arriving at corner 1
    map.paths[1].ends[1].vehicleRule = 1; // road 1 arriving at corner 1
    linkIntersections(map);
    return map;
}

// A crossroads at the origin with four two-lane roads coming in from 200 m
// away, listed by angle as MM2 bakes them: west, south, east, north.
city::AiMap crossroads() {
    city::AiMap map;
    const Vec3 far[4] = {{-200, 0, 0}, {0, 0, 200}, {200, 0, 0}, {0, 0, -200}};
    map.intersections.push_back({0, 0, {0, 0, 0}, {0, 1, 2, 3}});
    for (int k = 0; k < 4; ++k) {
        const Vec3 near = far[k].normalized() * 15.0f;
        map.paths.push_back(road(k, far[k], near, static_cast<std::uint32_t>(k + 1), 0, 2));
        map.intersections.push_back(
            {static_cast<std::uint16_t>(k + 1), 0, far[k], {static_cast<std::uint32_t>(k)}});
    }
    linkIntersections(map);
    return map;
}

ai::VehicleData sedan() {
    ai::VehicleData d;
    d.model = "test";
    d.size = {2.0f, 1.5f, 4.5f};
    return d;
}

} // namespace

TEST(AiRoute, IntersectionOrderAndLaneRules) {
    const auto map = crossroads();
    const auto net = ai::RoadNetwork::build(map, {});
    ai::Random rng(3);
    // A car from the south (road 1) heading north towards the crossroads.
    std::map<int, std::set<std::pair<int, int>>> picks; // lane -> {(road, next lane)}
    for (int lane = 0; lane < 2; ++lane)
        for (int i = 0; i < 200; ++i) {
            ai::RailLink next;
            ASSERT_TRUE(ai::chooseNextLaneLink(net, {1, 1, lane}, rng, next));
            EXPECT_EQ(next.dir, -1); // leaving the crossroads, towards ends[1]
            picks[lane].insert({next.path, next.lane});
        }
    // Lane 0 (the leftmost) turns left into the west road or goes straight on
    // north, into lane 0; the last lane turns right (east) or goes straight,
    // into the last lane (aiMap::ChooseNextLeftStraightLink / RightStraightLink).
    EXPECT_EQ(picks[0], (std::set<std::pair<int, int>>{{0, 0}, {3, 0}}));
    EXPECT_EQ(picks[1], (std::set<std::pair<int, int>>{{2, 1}, {3, 1}}));
    // The path after this one in the intersection's list is to its right.
    EXPECT_EQ(ai::solveTurnType(net, {1, 1, 0}, 2, -1), ai::TurnType::Right);
    EXPECT_EQ(ai::solveTurnType(net, {1, 1, 0}, 0, -1), ai::TurnType::Left);
    EXPECT_EQ(ai::solveTurnType(net, {1, 1, 0}, 3, -1), ai::TurnType::Straight);
}

TEST(AiRoute, ClosedSidesAreNeverChosen) {
    auto map = crossroads();
    map.paths[0].left.roadType = 1; // nothing may leave westwards
    const auto net = ai::RoadNetwork::build(map, {});
    ai::Random rng(5);
    for (int i = 0; i < 200; ++i) {
        ai::RailLink next;
        ASSERT_TRUE(ai::chooseNextLaneLink(net, {1, 1, 0}, rng, next));
        EXPECT_NE(next.path, 0);
    }
}

TEST(AiRoadNetwork, DriveOnLeftReversesRoadsAsMM2) {
    const auto map = crossroads();
    ai::NetworkOptions opts;
    opts.driveOnLeft = true;
    const auto net = ai::RoadNetwork::build(map, opts);
    // The south road, northbound (direction +1): lane 0 is the leftmost lane,
    // now the outer one on the west side of the centre line.
    const auto& info = net.paths()[1];
    const auto& lane0 = net.lanes()[static_cast<std::size_t>(info.lanesOf(1)[0])];
    const auto& lane1 = net.lanes()[static_cast<std::size_t>(info.lanesOf(1)[1])];
    EXPECT_NEAR(lane0.line.points.front().x, -6.0f, 1e-4f);
    EXPECT_NEAR(lane1.line.points.front().x, -2.0f, 1e-4f);
    EXPECT_GT(lane0.line.points.front().z, lane0.line.points.back().z); // still northbound
    // Sidewalks stay where they are.
    const auto& walk = net.sidewalks()[static_cast<std::size_t>(info.sidewalks[1])];
    EXPECT_NEAR(walk.centre.points.front().x, 9.5f, 1e-4f);
}

// OpenMM2: the density changes during a race. Off has no cars; a density again fills the area.
TEST(AiTraffic, DensityChangesRebuildThePool) {
    const auto map = square();
    const auto net = ai::RoadNetwork::build(map, {});
    ai::TrafficLights lights;
    lights.build(net);
    ai::TrafficSettings settings;
    settings.density = 1.0f;
    ai::Traffic traffic(net, lights, {sedan()}, settings, 1);
    traffic.populateAll();
    traffic.step(ai::kAiStepSeconds, {1000, 0, 1000}, {}, 0);
    ASSERT_GT(traffic.cars().size(), 0u);

    traffic.setDensity(0.0f);
    traffic.reset();
    traffic.step(ai::kAiStepSeconds, {1000, 0, 1000}, {}, 0);
    EXPECT_FLOAT_EQ(traffic.density(), 0.0f);
    EXPECT_EQ(traffic.cars().size(), 0u);

    traffic.setDensity(1.0f);
    traffic.reset();
    traffic.populateAll();
    traffic.step(ai::kAiStepSeconds, {1000, 0, 1000}, {}, 0);
    EXPECT_GT(traffic.cars().size(), 0u);
    EXPECT_EQ(traffic.poolFree(), ai::kAmbientPoolSize - static_cast<int>(traffic.cars().size()));
}

TEST(AiTraffic, DensityOfANewArea) {
    // aiMap::AdjustAmbients: about density * 0.2 / 8 cars per metre of usable
    // lane (centre length - 5 m per lane) when the area is populated.
    const auto map = square();
    const auto net = ai::RoadNetwork::build(map, {});
    ai::TrafficLights lights;
    lights.build(net);
    ai::TrafficSettings settings;
    settings.density = 1.0f;
    ai::Traffic traffic(net, lights, {sedan()}, settings, 1);
    traffic.populateAll();
    traffic.step(ai::kAiStepSeconds, {1000, 0, 1000}, {}, 0);
    const float usable = 4.0f * 2.0f * (200.0f - 5.0f);
    const int expected = static_cast<int>(usable * 0.2f / 8.0f);
    EXPECT_NEAR(static_cast<int>(traffic.cars().size()), expected, 1);
    EXPECT_EQ(traffic.poolFree(), ai::kAmbientPoolSize - static_cast<int>(traffic.cars().size()));
}

TEST(AiTraffic, CarsStopAtRedAndGoOnGreen) {
    const auto map = square();
    const auto net = ai::RoadNetwork::build(map, {});
    ai::TrafficLights lights;
    lights.build(net);
    lights.forceAll(ai::LightState::Red);
    ai::TrafficSettings settings;
    settings.density = 1.0f;
    ai::Traffic traffic(net, lights, {sedan()}, settings, 42);
    traffic.populateAll();
    const Vec3 player{1000, 0, 1000}; // far away
    for (int i = 0; i < 30 * 90; ++i)
        traffic.step(ai::kAiStepSeconds, player, {}, 0);
    // Every car arriving at the red corner waits before the line; nobody is
    // in the corner's turn.
    int queued = 0;
    for (const auto& car : traffic.cars()) {
        const auto d = traffic.debug(car.id);
        ASSERT_GE(d.lane, 0);
        const auto& lane = net.lanes()[static_cast<std::size_t>(d.lane)];
        if (lane.toIntersection != 1)
            continue;
        EXPECT_FALSE(d.turning);
        EXPECT_LT(d.s, lane.line.length);
        if (car.speed < 0.05f)
            ++queued;
    }
    EXPECT_GT(queued, 2);
    // Back to cycling: traffic goes through the corner again.
    lights.reset();
    std::set<int> crossed;
    for (int i = 0; i < 30 * 60; ++i) {
        lights.update(ai::kAiStepSeconds);
        traffic.step(ai::kAiStepSeconds, player, {}, 0);
        for (const auto& car : traffic.cars()) {
            const auto d = traffic.debug(car.id);
            if (d.turning && d.lane >= 0 && net.lanes()[static_cast<std::size_t>(d.lane)].toIntersection == 1)
                crossed.insert(car.id);
        }
    }
    EXPECT_GT(crossed.size(), 2u);
}

TEST(AiTraffic, CarsKeepTheirDistance) {
    // Round and round the block: no car ever drives into the one ahead.
    const auto map = square();
    const auto net = ai::RoadNetwork::build(map, {});
    ai::TrafficLights lights;
    lights.build(net);
    ai::TrafficSettings settings;
    settings.density = 1.0f;
    ai::Traffic traffic(net, lights, {sedan()}, settings, 7);
    traffic.populateAll();
    int minDistanceViolations = 0;
    for (int i = 0; i < 30 * 120; ++i) {
        lights.update(ai::kAiStepSeconds);
        traffic.step(ai::kAiStepSeconds, {1000, 0, 1000}, {}, 0);
        for (const auto& car : traffic.cars()) {
            const auto d = traffic.debug(car.id);
            if (d.lead >= 0 && !d.turning && !d.changingLane && d.leadDistance < 4.0f)
                ++minDistanceViolations;
        }
    }
    EXPECT_EQ(minDistanceViolations, 0);
}

TEST(Mm1VehicleData, ReadsMm1sAiVehicleDataBlock) {
    // The shape of mm1/tune/vasedans.aivehicledata (MM1's own values).
    const char* text = "aiVehicleData :18a48998 {\n"
                       "    NodeName \"vasedans\"\n"
                       "    Size 1.839 1.123 4.287\n"
                       "    Mass 885.095 \n"
                       "    Friction 0.5 \n"
                       "    SuspensionLimit 0.07 \n"
                       "    TireRadius 0.301425 \n"
                       "    FLOff -0.7815 0.2965 -1.458\n"
                       "    FROff 0.776\t0.2965 -1.458\n"
                       "    BLOff 0.7628 0.2965 1.165\n"
                       "    BROff -0.7628 0.2965 1.165\n"
                       "}\n";
    const auto d = ai::parseMm1VehicleData("VASEDANS", text);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->model, "vasedans");
    EXPECT_FLOAT_EQ(d->size.z, 4.287f);
    EXPECT_FLOAT_EQ(d->mass, 885.095f);
    EXPECT_FLOAT_EQ(d->limit, 0.07f);
    EXPECT_FLOAT_EQ(d->wheelRadius, 0.301425f);
    EXPECT_EQ(d->wheelCount, 4);
    EXPECT_FLOAT_EQ(d->wheels[0].x, -0.7815f);
    EXPECT_FLOAT_EQ(d->wheels[1].x, 0.776f); // a tab between the numbers
    EXPECT_FLOAT_EQ(d->wheels[3].z, 1.165f);
    EXPECT_FLOAT_EQ(d->cg.y, 1.123f * 0.5f); // inferred: MM1 gives none
    EXPECT_FALSE(ai::parseMm1VehicleData("x", "nothing here"));
}
