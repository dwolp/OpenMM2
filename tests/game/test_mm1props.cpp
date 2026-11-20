// OpenMM2 Enhanced: MM1's props (mmBangerData) through MM2's banger data.
#include "game/bangers/BangerData.h"
#include "game/bangers/BangerSet.h"

#include <gtest/gtest.h>

using namespace mm2::game::bangers;

namespace {

// MM1's tune/tpltst.mmbangerdata (a street light), as shipped.
constexpr std::string_view kStreetLight = R"(mmBangerData :05e1f360 {
    NodeName "tpltst"
    AudioId 3 
    Size 5.876 12.64 0.7594
    CG -2.786 6.322 -4.917e-007
    Offset 0 0 0
    GlowOffset -2.26 6.27 0
    Mass 2000 
    Elasticity 0.5 
    Friction 0.9 
    ImpulseLimit2 9.6e+007 
    SpinAxis 0 
    Flash 0 
    CollisionType 4 
    NumParts 2 
    PartNames [
"BREAK_PROPLIGHT01"         
"BREAK_PROPLIGHT02"         
        ]
    BirthRule asBirthRule :05e1f3ec {
            Position 0 0 0
            PositionVar 0 0 0
            Velocity 0 0 0
            VelocityVar 0 0 0
            Life 1 
            LifeVar 0 
            Mass 1 
            MassVar 0 
            Radius 1 
            RadiusVar 0 
            Drag 0 
            DragVar 0 
            DRadius 0 
            DRadiusVar 0 
            DAlpha 0 
            DAlphaVar 0 
            DRotation 0 
            DRotationVar 0 
            InitialBlast 0 
            SpewRate 0 
            SpewTimeLimit 0 
            Gravity -9.8 
            TexFrameStart 0 
            TexFrameEnd 0 
            BirthFlags 0 
        }
    TexNumber 0 
    BillFlags 32
    YRadius 0.152 
}
)";

} // namespace

TEST(Mm1Props, Mm2ReadsMm1BangerData) {
    std::string err;
    auto d = parseMm1BangerData("tpltst", kStreetLight, &err);
    ASSERT_TRUE(d) << err;
    EXPECT_FLOAT_EQ(d->size.y, 12.64f);
    EXPECT_FLOAT_EQ(d->cg.x, -2.786f);
    EXPECT_FLOAT_EQ(d->mass, 2000.0f);
    // MM1's CollisionType 4 (a prop colliding with terrain and movers) as
    // MM2's 0x10, and MM1's bound (bnd/<name>_bnd.bnd) for it.
    EXPECT_EQ(d->collisionType, 0x10);
    EXPECT_TRUE(d->mm1);
    EXPECT_EQ(d->numParts, 2);
    EXPECT_EQ(d->billFlags, 32);
    EXPECT_FLOAT_EQ(d->yRadius, 0.152f);
    ASSERT_EQ(d->glowOffsets.size(), 1u);
    EXPECT_FLOAT_EQ(d->glowOffsets[0].x, -2.26f);
    ASSERT_TRUE(d->birthRule);
}

// MM1's trees (BillFlags 31) are billboards at every level of detail
// (mmBangerInstance::Draw); the street light (32) never is.
TEST(Mm1Props, BillboardFlagsPerLevelOfDetail) {
    using mm2::asset::Lod;
    for (Lod lod : {Lod::High, Lod::Medium, Lod::Low, Lod::VeryLow})
        EXPECT_TRUE(mm1Billboard(31, lod));
    EXPECT_FALSE(mm1Billboard(32, Lod::High));
    EXPECT_TRUE(mm1Billboard(0x8, Lod::High));
    EXPECT_FALSE(mm1Billboard(0x8, Lod::Medium));
    EXPECT_TRUE(mm1Billboard(0x1, Lod::VeryLow));
    EXPECT_FALSE(mm1Billboard(31, Lod::None));
}

// agiViewParameters::SetBillY: the card's +Z turns to the eye about its up
// axis, whatever the eye's height.
TEST(Mm1Props, BillboardTurnsAboutTheUpAxis) {
    mm2::Mat34 m;
    m.m0 = {1, 0, 0};
    m.m1 = {0, 1, 0};
    m.m2 = {0, 0, 1};
    m.m3 = {10, 2, 10};
    const mm2::Mat34 b = billboardY(m, {20, 30, 10});
    EXPECT_NEAR(b.m2.x, 1.0f, 1e-6f);
    EXPECT_NEAR(b.m2.y, 0.0f, 1e-6f);
    EXPECT_NEAR(b.m2.z, 0.0f, 1e-6f);
    EXPECT_NEAR(b.m0.x, 0.0f, 1e-6f);
    EXPECT_NEAR(b.m0.z, -1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(b.m1.y, 1.0f);
    EXPECT_FLOAT_EQ(b.m3.x, 10.0f);
    // Straight above: unchanged.
    const mm2::Mat34 up = billboardY(m, {10, 50, 10});
    EXPECT_FLOAT_EQ(up.m2.z, 1.0f);
}
