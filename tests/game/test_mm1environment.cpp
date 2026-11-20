// OpenMM2 Enhanced: an MM1 city's own snow lighting (CityData::snowLighting).
#include "game/CityRenderer.h"

#include <gtest/gtest.h>

using namespace mm2;
using namespace mm2::game;

namespace {

city::CityData cityWithRainAndSnow(bool snow) {
    city::CityData c;
    c.fog.assign(static_cast<std::size_t>(city::kTimesOfDay * city::kWeathers), city::FogDef{});
    city::LightingDef rain;
    rain.keyColor = {0.2f, 0.2f, 0.2f};
    rain.ambient = 0xFF101010u;
    const auto rainIndex = static_cast<std::size_t>(city::lightingIndex(1, 3));
    c.lighting[rainIndex] = rain;
    c.fog[rainIndex].end = 300.0f;
    c.fog[rainIndex].r = 10;
    if (snow) {
        city::LightingDef s;
        s.keyColor = {0.8f, 0.8f, 0.9f};
        s.ambient = 0xFF808080u;
        c.snowLighting[1] = s;
        c.snowFog[1].end = 150.0f;
        c.snowFog[1].r = 200;
    }
    return c;
}

} // namespace

TEST(Mm1Environment, SnowUsesTheCitysSnowTables) {
    EnvironmentOptions o;
    o.lightQuality = 3;
    o.farClip = 1000.0f;
    const auto c = cityWithRainAndSnow(true);
    const Environment env = makeEnvironment(c, TimeOfDay::Noon, Weather::Snow, o);
    EXPECT_TRUE(env.snow);
    EXPECT_FLOAT_EQ(env.frame.lights[0].color.x, 0.8f);
    EXPECT_FLOAT_EQ(env.frame.fogEnd, 150.0f);
    EXPECT_NEAR(env.frame.fogColor.x, 200.0f / 255.0f, 1e-6f);
    // Rain keeps the rain table.
    const Environment rain = makeEnvironment(c, TimeOfDay::Noon, Weather::Rain, o);
    EXPECT_FALSE(rain.snow);
    EXPECT_FLOAT_EQ(rain.frame.lights[0].color.x, 0.2f);
}

TEST(Mm1Environment, WithoutSnowTablesSnowIsRain) {
    EnvironmentOptions o;
    o.lightQuality = 3;
    o.farClip = 1000.0f;
    const auto c = cityWithRainAndSnow(false);
    const Environment env = makeEnvironment(c, TimeOfDay::Noon, Weather::Snow, o);
    EXPECT_FALSE(env.snow);
    EXPECT_FLOAT_EQ(env.frame.lights[0].color.x, 0.2f);
    EXPECT_FLOAT_EQ(env.frame.fogEnd, 300.0f);
}
