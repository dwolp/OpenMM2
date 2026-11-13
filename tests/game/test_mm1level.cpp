// OpenMM2 Enhanced: MM1 cities' collision (game::Mm1Level).
#include "game/Mm1Level.h"

#include <gtest/gtest.h>

using mm2::game::Mm1Level;

TEST(Mm1Level, MapsMm1MaterialsToMm2ByName) {
    EXPECT_EQ(Mm1Level::mm2MaterialFor("TBPARK03GRASS"), "grass");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("T_GRASS"), "grass");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("BUSH"), "grass");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("DIRT"), "dirt");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("T_WATER"), "deepwater");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("FLAGSTONE"), "cobblestone");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("RD2LANE_COB"), "cobblestone");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("SDWLK"), "_default");
    EXPECT_EQ(Mm1Level::mm2MaterialFor("RINTER"), "_default");
}
