// OpenMM2 Enhanced: MM1's props (mmBangerData) through MM2's banger data.
#include "game/bangers/BangerData.h"

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
    EXPECT_EQ(d->collisionType, 4);
    EXPECT_EQ(d->numParts, 2);
    EXPECT_EQ(d->billFlags, 32);
    EXPECT_FLOAT_EQ(d->yRadius, 0.152f);
    ASSERT_EQ(d->glowOffsets.size(), 1u);
    EXPECT_FLOAT_EQ(d->glowOffsets[0].x, -2.26f);
    ASSERT_TRUE(d->birthRule);
}
