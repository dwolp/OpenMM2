// OpenMM2 Enhanced: MM1's tune/<car>.mmcarsim (an invented sample in the
// retail layout: object headers with a class and an address, lists, nested
// records and fields the parser skips).
#include "phys/vehicle/Mm1CarParams.h"

#include <gtest/gtest.h>

using namespace mm2;
using namespace mm2::phys;

namespace {

constexpr std::string_view kSample = R"(mmCarSim :00001000 {
    Mass 1300
    InertiaBox 2.5 2.3 3.1
    BodyCG 0 -0.07 -0.01
    Drag 0.2
    DrivetrainType 1
    SlipPercentThresh 0.05
    SteerMultiplier 1.5
    DashCamHeadlightOffset 0 1 -2
    Engine mmEngine :00002000 {
            MaxHorsePower 240
            OptRPM 5800
            GCL 0.25
        }
    Trans mmTransmission :00003000 {
            NumGears 3
            GearRatios [
                 -19.8
                 0
                 21
                ]
            UpshiftRPM [ 6000 6300 ]
            DownshiftBias 1.55
        }
    AeroCollide asAero :00004000 {
            AngVelDamp 6 0 6
        }
    FrontLeft mmWheel :00005000 {
            Spring 66300
            StaticFric 1.3
            RubberSpringLat 60000
        }
    BackLeft mmWheel :00006000 {
            Spring 55000
        }
    AsphaltRule asBirthRule :00007000 {
            Position 1 2 3
            Mass 0.1
        }
    AsphaltPtx 0.5
}
)";

TEST(Mm1CarParams, ReadsTheFields) {
    std::string err;
    const auto p = parseMm1CarParams(kSample, &err);
    ASSERT_TRUE(p) << err;
    EXPECT_EQ(p->className, "mmCarSim");
    EXPECT_FLOAT_EQ(p->mass, 1300.0f);
    EXPECT_FLOAT_EQ(p->inertiaBox.z, 3.1f);
    EXPECT_FLOAT_EQ(p->bodyCG.y, -0.07f);
    EXPECT_EQ(p->drivetrainType, 1);
    EXPECT_FLOAT_EQ(p->steerMultiplier, 1.5f);
    ASSERT_TRUE(p->dashCamHeadlightOffset);
    EXPECT_FLOAT_EQ(p->dashCamHeadlightOffset->z, -2.0f);
    EXPECT_FALSE(p->povCamHeadlightOffset);
    EXPECT_FLOAT_EQ(p->engine.maxHorsePower, 240.0f);
    EXPECT_FLOAT_EQ(p->engine.gcl, 0.25f);
    EXPECT_FLOAT_EQ(p->aero.angVelDamp.x, 6.0f);
    EXPECT_FLOAT_EQ(p->aero.angVelDamp.y, 0.0f);
}

TEST(Mm1CarParams, ReadsListsAcrossLinesAndOnOne) {
    const auto p = parseMm1CarParams(kSample);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->trans.numGears, 3);
    ASSERT_EQ(p->trans.gearRatios.size(), 3u);
    EXPECT_FLOAT_EQ(p->trans.gearRatios[0], -19.8f);
    EXPECT_FLOAT_EQ(p->trans.gearRatios[2], 21.0f);
    ASSERT_EQ(p->trans.upshiftRPM.size(), 2u);
    EXPECT_FLOAT_EQ(p->trans.upshiftRPM[1], 6300.0f);
    EXPECT_FLOAT_EQ(p->trans.downshiftBias, 1.55f);
}

TEST(Mm1CarParams, ReadsTheWheelsAndSkipsTheRules) {
    const auto p = parseMm1CarParams(kSample);
    ASSERT_TRUE(p);
    EXPECT_FLOAT_EQ(p->frontLeft.spring, 66300.0f);
    EXPECT_FLOAT_EQ(p->frontLeft.staticFric, 1.3f);
    EXPECT_TRUE(p->frontLeft.hasLat);
    EXPECT_FLOAT_EQ(p->frontLeft.rubberSpringLat, 60000.0f);
    EXPECT_FLOAT_EQ(p->backLeft.spring, 55000.0f);
    EXPECT_FALSE(p->backLeft.hasLat);
    // The particle rule's Mass (0.1) is not the car's.
    EXPECT_FLOAT_EQ(p->mass, 1300.0f);
}

TEST(Mm1CarParams, RejectsTextWithoutTheBlock) {
    std::string err;
    EXPECT_FALSE(parseMm1CarParams("Mass 5\n", &err));
    EXPECT_FALSE(err.empty());
}

} // namespace
