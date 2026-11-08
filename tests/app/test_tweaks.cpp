#include "app/Tweaks.h"

#include <gtest/gtest.h>

#include <set>

using namespace mm2;
using namespace mm2::app;

// Every tweak defaults to the original look, has a valid default and a
// unique key.
TEST(Tweaks, TableDefaultsToTheOriginal) {
    std::set<std::string_view> keys;
    for (const Tweak& t : kTweaks) {
        EXPECT_EQ(t.fallback, 0) << t.key;
        EXPECT_GE(t.count, 2) << t.key;
        EXPECT_LE(t.count, static_cast<int>(t.choices.size())) << t.key;
        EXPECT_TRUE(keys.insert(t.key).second) << t.key;
        EXPECT_FALSE(t.label.empty()) << t.key;
    }
    IniFile empty;
    for (const Tweak& t : kTweaks)
        EXPECT_FALSE(tweakOn(empty, t.key)) << t.key;
}

TEST(Tweaks, ReadsClampsAndCopies) {
    IniFile ini;
    ini.parse("[Tweaks]\nEnhancedCarLighting=1\n");
    EXPECT_TRUE(tweakOn(ini, "EnhancedCarLighting"));
    ini.parse("[Tweaks]\nEnhancedCarLighting=7\n");
    EXPECT_EQ(tweakValue(ini, "EnhancedCarLighting"), 1); // clamped to the last choice
    ini.parse("[Tweaks]\nEnhancedCarLighting=-3\n");
    EXPECT_EQ(tweakValue(ini, "EnhancedCarLighting"), 0);
    EXPECT_EQ(tweakValue(ini, "NoSuchTweak"), 0);

    IniFile from, to;
    from.setInt(kTweaksSection, "EnhancedCarLighting", 1);
    to.set("Graphics", "FarClip", "500");
    copyTweaks(from, to);
    EXPECT_TRUE(tweakOn(to, "EnhancedCarLighting"));
    EXPECT_EQ(to.getString("Graphics", "FarClip"), "500"); // other sections untouched
    copyTweaks(IniFile{}, to);
    EXPECT_FALSE(to.get(kTweaksSection, "EnhancedCarLighting").has_value()); // removed, so the default
}

// All image-filter tweaks off is no filtering at all (composite.frag's plain
// copy); each one switches the filters on.
TEST(Tweaks, ImageFiltersOffByDefault) {
    EXPECT_FALSE(postProcessFromTweaks(IniFile{}).active());
    for (std::string_view key : {"ColorGrade", "Sharpen", "Fxaa", "Vignette", "Bloom", "AmbientOcclusion"}) {
        IniFile ini;
        ini.setInt(kTweaksSection, key, 1);
        EXPECT_TRUE(postProcessFromTweaks(ini).active()) << key;
    }
    IniFile ini;
    ini.setInt(kTweaksSection, "Sharpen", 99); // clamped to High
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).sharpen, 0.8f);
}
