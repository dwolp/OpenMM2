// MM1's drawbridges (mmBridgeSet / mmAnimDOF) in game::world::Mm1Bridges.
#include "game/world/Mm1Bridges.h"

#include "vfs/DirectoryFs.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace mm2;
using namespace mm2::game::world;

namespace {

using Part = Mm1Bridges::Part;

Part leaf() {
    Part p;
    p.kind = Part::Kind::Leaf;
    return p;
}

Part gate() {
    Part p;
    p.kind = Part::Kind::Gate;
    return p;
}

// A set as Chicago's: a leaf, two gates, the other leaf, two gates.
city::Mm1City cityWithOneBridge() {
    city::Mm1City c;
    city::Mm1BridgeSet set;
    set.name = "DrawBridge3";
    auto part = [](const char* model, Vec3 a, Vec3 b) { return city::Mm1GizmoPart{model, a, b}; };
    set.parts = {part("tpdrawbridge06", {162.6f, 0, -552.6f}, {177.6f, 0, -552.7f}),
                 part("tpcrossgate06", {177.6f, 0.15f, -551.6f}, {178.6f, 0.15f, -551.7f}),
                 part("tpcrossgate06", {147.6f, 0.15f, -551.4f}, {146.6f, 0.15f, -551.3f}),
                 part("tpdrawbridge06", {162.6f, 0, -617.4f}, {147.6f, 0, -617.4f}),
                 part("tpcrossgate06", {147.6f, 0.15f, -618.5f}, {146.6f, 0.15f, -618.5f}),
                 part("tpcrossgate06", {177.6f, 0.15f, -618.5f}, {178.6f, 0.15f, -618.5f})};
    c.bridges.push_back(set);
    return c;
}

} // namespace

TEST(Mm1Bridges, TuningNames) {
    Mm1Bridges::Options o;
    EXPECT_EQ(Mm1Bridges::tuningName(o), "roam");
    o.mode = game::GameMode::Checkpoint;
    o.raceIndex = 6;
    EXPECT_EQ(Mm1Bridges::tuningName(o), "race6");
    o.mode = game::GameMode::Circuit;
    o.raceIndex = 2;
    EXPECT_EQ(Mm1Bridges::tuningName(o), "circuit2");
    o.mode = game::GameMode::Blitz;
    o.multiplayer = true;
    EXPECT_EQ(Mm1Bridges::tuningName(o), "blitz2m");
}

TEST(Mm1Bridges, PartRisesAfterItsDelayAlongTheEasing) {
    city::Mm1BridgeTuning t;
    t.bridgeOnDelay = 1.5f;
    Part p = leaf();
    Mm1Bridges::setSwitch(p, true);
    EXPECT_EQ(p.state, Part::State::TurningOn);
    // The delay first: no progress for 1.5 s.
    for (int i = 0; i < 15; ++i)
        Mm1Bridges::updatePart(p, t, 0.1f);
    EXPECT_FLOAT_EQ(p.t, 0.0f);
    // Then Delta (0.2) per second: half way after 2.5 s more, eased to half
    // the angle.
    for (int i = 0; i < 26; ++i)
        Mm1Bridges::updatePart(p, t, 0.1f);
    EXPECT_NEAR(p.t, 0.5f, 0.021f);
    // Done at 1: on, at the goal angle (a leaf turns about its X axis: its
    // far end, -Z, rises).
    for (int i = 0; i < 40; ++i)
        Mm1Bridges::updatePart(p, t, 0.1f);
    EXPECT_EQ(p.state, Part::State::On);
    EXPECT_FLOAT_EQ(p.t, 1.0f);
    const Vec3 tip = p.matrix.transform({0, 0, -32.5f});
    EXPECT_NEAR(std::atan2(tip.y, -tip.z), t.bridgeOnGoal, 1e-4f);
}

TEST(Mm1Bridges, EasingIsHalfCosine) {
    city::Mm1BridgeTuning t;
    Part p = leaf();
    p.t = 0.25f;
    Mm1Bridges::place(p, t);
    const float ease = (1.0f - std::cos(0.25f * std::numbers::pi_v<float>)) * 0.5f;
    const Vec3 tip = p.matrix.transform({0, 0, -1.0f});
    EXPECT_NEAR(std::atan2(tip.y, -tip.z), t.bridgeOnGoal * ease, 1e-5f);
}

TEST(Mm1Bridges, GateLowersAcrossTheRoadAndRisesAfterItsDelay) {
    city::Mm1BridgeTuning t;
    Part g = gate();
    Mm1Bridges::setState(g, false);
    Mm1Bridges::place(g, t);
    // Off: upright (-pi/2 about Z turns the arm, along -X, up).
    const Vec3 arm = g.matrix.transform({-10.0f, 0, 0});
    EXPECT_NEAR(arm.x, 0.0f, 1e-4f);
    EXPECT_NEAR(std::abs(arm.y), 10.0f, 1e-4f);
    // On (the bridge rises): no delay, across the road after 1 / 0.6 s.
    Mm1Bridges::setSwitch(g, true);
    for (int i = 0; i < 17; ++i)
        Mm1Bridges::updatePart(g, t, 0.1f);
    EXPECT_EQ(g.state, Part::State::On);
    EXPECT_NEAR(g.matrix.transform({-10.0f, 0, 0}).x, -10.0f, 1e-4f);
    // Off again: it waits GateOffDelay (4.3 s) before rising.
    Mm1Bridges::setSwitch(g, false);
    for (int i = 0; i < 43; ++i)
        Mm1Bridges::updatePart(g, t, 0.1f);
    EXPECT_FLOAT_EQ(g.t, 1.0f);
    EXPECT_EQ(g.state, Part::State::TurningOff);
}

TEST(Mm1Bridges, RaceTuningTriggersNearTheBridge) {
    const auto city = cityWithOneBridge();
    vfs::Vfs vfs;
    Mm1Bridges::Options o;
    o.mode = game::GameMode::Checkpoint;
    o.raceIndex = 3;
    const auto t = Mm1Bridges::tuningFor(city, vfs, o);
    ASSERT_EQ(t.sets.size(), 1u);
    const auto& s = t.sets[0];
    EXPECT_TRUE(s.triggered);
    EXPECT_EQ(s.initialPos, 0);
    // The leaves' midpoint, four times their distance (as race6's 67185).
    EXPECT_NEAR(s.triggerPos.z, -585.0f, 1e-3f);
    EXPECT_NEAR(s.triggerDist2, 64.8f * 64.8f * 16.0f, 1.0f);
    EXPECT_TRUE(std::isinf(s.triggerTimeout));
    EXPECT_NEAR(t.bridgeOnGoal, 0.314159f, 1e-5f);
}

TEST(Mm1Bridges, CruiseAndNetworkStartUp) {
    const auto city = cityWithOneBridge();
    vfs::Vfs vfs;
    Mm1Bridges::Options o;
    auto t = Mm1Bridges::tuningFor(city, vfs, o);
    EXPECT_EQ(t.sets[0].initialPos, 1);
    EXPECT_FALSE(t.sets[0].triggered);
    o.mode = game::GameMode::Circuit;
    o.multiplayer = true;
    t = Mm1Bridges::tuningFor(city, vfs, o);
    EXPECT_EQ(t.sets[0].initialPos, 1);
    EXPECT_TRUE(std::isinf(t.sets[0].upInterval));
}

TEST(Mm1Bridges, SetCyclesAndTriggers) {
    // Without banger data the parts still move (no bodies).
    const auto city = cityWithOneBridge();
    const auto dir = std::filesystem::temp_directory_path() / "openmm2_mm1bridges";
    std::filesystem::create_directories(dir / "mm1" / "tune");
    {
        std::ofstream f(dir / "mm1" / "tune" / "race1.mmbridgemgr");
        f << "mmBridgeMgr :0 {\n    Bridges [\n        mmBridgeSet :0 {\n"
             "            DownInterval 2\n            UpInterval 1\n            Triggered 1\n"
             "            TriggerPos 162 0 -585\n            TriggerDist2 100\n        }\n    ]\n}\n";
    }
    vfs::Vfs vfs;
    vfs.mount(std::make_shared<vfs::DirectoryFs>(dir));
    game::bangers::BangerDataLibrary data(vfs);
    game::bangers::BangerSet bounds(data);
    Mm1Bridges::Options o;
    o.mode = game::GameMode::Checkpoint;
    o.raceIndex = 1;
    Mm1Bridges b(city, vfs, data, bounds, nullptr, o);
    ASSERT_EQ(b.sets().size(), 1u);
    EXPECT_EQ(b.sets()[0].state, 3);
    // Far away: still waiting.
    for (int i = 0; i < 100; ++i)
        b.update(0.1f, Vec3{0, 0, 0});
    EXPECT_EQ(b.sets()[0].state, 3);
    // Within 10 m: the parts switch up and the set moves.
    b.update(0.1f, Vec3{162, 0, -585});
    EXPECT_EQ(b.sets()[0].state, 2);
    EXPECT_EQ(b.sets()[0].switchOn, 1);
    // Up within a few seconds (leaves: 0.9 s delay, 5 s turning).
    for (int i = 0; i < 65; ++i)
        b.update(0.1f, std::nullopt);
    EXPECT_EQ(b.sets()[0].state, 1);
    // Up for 1 s, then lowering.
    for (int i = 0; i < 12; ++i)
        b.update(0.1f, std::nullopt);
    EXPECT_EQ(b.sets()[0].state, 2);
    EXPECT_EQ(b.sets()[0].switchOn, 0);
    // The gate posts are props.
    EXPECT_EQ(b.gateBases().size(), 4u);
    b.reset();
    EXPECT_EQ(b.sets()[0].state, 3);
    std::filesystem::remove_all(dir);
}
