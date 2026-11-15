// OpenMM2 Enhanced: a car on Midtown Madness 1's physics (docs/mm1physics.md).
// The tuning is a Beetle-sized sample in the retail layout.
#include "phys/World.h"
#include "phys/vehicle/CarSim.h"
#include "phys/vehicle/Controls.h"
#include "phys/vehicle/Mm1CarParams.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace mm2;
using namespace mm2::phys;

namespace {

constexpr std::string_view kSample = R"(mmCarSim :00001000 {
    Mass 1300
    InertiaBox 2.5 2.3 3.1
    BodyCG 0 -0.07 -0.01
    Drag 0.2
    Downforce 0
    BoundFriction 0.2
    BoundElasticity 0.3
    DriftTorque 2.5
    DrivetrainType 1
    CarFrictionHandling 1
    MaxDamage 600000
    MedDamage 198000
    RedistHeight 1
    RedistLongRatio 0.5
    Engine mmEngine :00002000 {
        MaxHorsePower 240
        OptRPM 5800
        MaxRPM 8500
        GCL 0.25
    }
    Trans mmTransmission :00003000 {
        NumGears 7
        GearRatios [
            -19.8
            0
            21
            15
            10
            7
            4.6
            2.54
        ]
        UpshiftRPM [ 6000 6000 6300 6500 6000 6000 6000 6000 ]
        DownshiftRPM [ 2000 2000 2000 2000 2000 2000 2000 2000 ]
        ManualNumGears 7
        ManualGearRatios [ -19.8 0 21 15 10 7 4.6 2.54 ]
        DownshiftBias 1.55
    }
    AeroCollide asAero :00004000 {
        AngVelDamp 6 0 6
        AngVel2Damp 6 0 6
    }
    FrontLeft mmWheel :00005000 {
        Spring 66300
        Damping 6200
        SteeringRatio 0.45
        SuspensionBlend 0.5
        BrakeRatio 0.6
        StaticFriction 2
        SuspensionLimit 0.3
        RenderableSuspensionLimit 0.1
        RubberSpring 40000
        RubberDamp 2000
        OptimumSlipPercent 0.03
        StaticFric 1.3
        SlidingFric 1
        RubberSpringLat 60000
        RubberDampLat 5000
    }
    BackLeft mmWheel :00006000 {
        Spring 66300
        Damping 6200
        SteeringRatio 0.01
        SuspensionBlend 0.5
        BrakeRatio 0.8
        StaticFriction 1
        SuspensionLimit 0.3
        RenderableSuspensionLimit 0.1
        RubberSpring 40000
        RubberDamp 2000
        OptimumSlipPercent 0.03
        StaticFric 1.1
        SlidingFric 0.81
        RubberSpringLat 60000
        RubberDampLat 5000
    }
    DriftThreshold 0.26
    SpinThreshold 1
    BrakeFrontFricMultiplier 0.2
    BrakeBackFricMultiplier 0.2
    SteerMultiplier 1
    SpinStart 0.9
    SpinStop 1.2
    SpinStartTime 0.2
    SpinEndTime 0.35
    SpinFromMax 0.4
    SpinFromMin 0
    SpinToMin 0.6
    SpinToMax 1
    FrontDriftFricMultiplier 0.84
    BackDriftFricMultiplier 0.85
    FrontSpinFricMultiplier 1
    BackSpinFricMultiplier 0.9
    LongSlideMultiplier 1
}
)";

VehicleGeometry geometry() {
    VehicleGeometry g;
    const float x[4] = {-0.754f, 0.754f, -0.754f, 0.754f};
    const float z[4] = {-1.184f, -1.184f, 1.236f, 1.236f};
    for (int i = 0; i < 4; ++i)
        g.wheels[static_cast<std::size_t>(i)] = {{x[i], 0.307f, z[i]}, 0.336f, 0.255f, true};
    g.body = Aabb{{-0.92f, 0.245f, -1.98f}, {0.94f, 1.56f, 2.01f}};
    return g;
}

PolygonSoup flatGround(float half = 20000.0f) {
    SoupGeometry g;
    g.vertices = {{-half, 0, -half}, {-half, 0, half}, {half, 0, half}, {half, 0, -half}};
    g.polys.push_back({{0, 1, 2, 3}, 4, 0});
    g.materialNames = {"default"};
    PolygonSoup soup;
    soup.add(g, Mat34::identity(), MaterialTable{});
    soup.finalize(2048.0f);
    return soup;
}

struct Mm1Car {
    World world;
    std::unique_ptr<CarSim> car = std::make_unique<CarSim>();
    ArcadeControls controls;
    std::optional<Mm1CarParams> mm1;

    Mm1Car() {
        CarSimParams p;
        mm1 = parseMm1CarParams(kSample);
        EXPECT_TRUE(mm1.has_value());
        p.mm1 = &*mm1;
        world.setStatic(flatGround());
        car->init(p, geometry());
        car->reset(Mat34::identity());
        world.add(&car->body);
    }
    void run(float seconds, const PedalInput& in) {
        const int n = static_cast<int>(seconds / kFixedSampleStep + 0.5f);
        for (int i = 0; i < n; ++i) {
            controls.apply(*car, in);
            world.step(kFixedSampleStep);
        }
    }
    float height() const { return car->body.ics.matrix.m3.y; }
};

} // namespace

TEST(Mm1Car, SettlesOnItsWheels) {
    Mm1Car c;
    c.run(3.0f, {});
    EXPECT_EQ(c.car->wheelsOnGround(), 4);
    EXPECT_LT(c.car->speedMph(), 0.5f);
    // The static weight share of each wheel, summed: the car's weight.
    float load = 0.0f;
    for (const Wheel& w : c.car->wheels)
        load += w.currentLoad;
    EXPECT_NEAR(load, 1300.0f * 19.8f, 1300.0f * 19.8f * 0.15f);
    EXPECT_GT(c.height(), 0.0f);
    EXPECT_LT(c.height(), 1.5f);
}

TEST(Mm1Car, AcceleratesAndBrakes) {
    Mm1Car c;
    c.run(1.0f, {});
    PedalInput go;
    go.accelerator = 1.0f;
    c.run(5.0f, go);
    const float fast = c.car->speedMph();
    EXPECT_GT(fast, 25.0f);
    EXPECT_TRUE(std::isfinite(fast));
    // Forward is -Z.
    EXPECT_LT(c.car->body.ics.matrix.m3.z, -20.0f);
    // Full brake: MM1 cuts the tyres' friction to BrakeFront/BackFricMultiplier
    // (0.2) above 0.8; the car still sheds most of its speed in 2.5 s. (The
    // arcade controls select reverse once it has stopped.)
    PedalInput stop;
    stop.brake = 1.0f;
    c.run(2.5f, stop);
    EXPECT_LT(c.car->speedMph(), fast * 0.4f);
}

TEST(Mm1Car, TurnsAndStaysUpright) {
    Mm1Car c;
    c.run(1.0f, {});
    PedalInput go;
    go.accelerator = 1.0f;
    c.run(3.0f, go);
    go.steering = 0.3f;
    c.run(3.0f, go);
    const Mat34& m = c.car->body.ics.matrix;
    // Turned (heading changed), not rolled over, still finite.
    EXPECT_GT(std::abs(m.m2.x), 0.1f);
    EXPECT_GT(m.m1.y, 0.7f);
    EXPECT_TRUE(std::isfinite(m.m3.x) && std::isfinite(m.m3.z));
}
