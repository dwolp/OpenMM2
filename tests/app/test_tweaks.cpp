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
    for (std::string_view key : {"ColorGrade", "Sharpen", "Fxaa", "Vignette", "Bloom", "AmbientOcclusion", "WetRoads", "SunShafts", "MotionBlur"}) {
        IniFile ini;
        ini.setInt(kTweaksSection, key, 1);
        EXPECT_TRUE(postProcessFromTweaks(ini).active()) << key;
    }
    IniFile ini;
    ini.setInt(kTweaksSection, "Sharpen", 99); // clamped to High
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).sharpen, 0.8f);
}

// Every slider fine-tunes a choice listed before it, and its presets lie in
// its range.
TEST(Tweaks, SlidersFollowTheirChoice) {
    for (std::size_t i = 0; i < kTweaks.size(); ++i) {
        const Tweak& t = kTweaks[i];
        if (t.kind != TweakKind::Slider)
            continue;
        const Tweak* parent = findTweak(t.parent);
        ASSERT_NE(parent, nullptr) << t.key;
        EXPECT_EQ(parent->kind, TweakKind::Choice) << t.key;
        EXPECT_LT(parent, &t) << t.key; // the page shows sliders under their choice
        EXPECT_LT(t.min, t.max) << t.key;
        EXPECT_GT(t.step, 0.0f) << t.key;
        for (int c = 0; c < parent->count; ++c) {
            EXPECT_GE(t.presets[static_cast<std::size_t>(c)], t.min) << t.key << " " << c;
            EXPECT_LE(t.presets[static_cast<std::size_t>(c)], t.max) << t.key << " " << c;
        }
    }
}

TEST(Tweaks, OverridesClampAndPresetsClearThem) {
    IniFile ini;
    const Tweak& bloom = *findTweak("Bloom");
    const Tweak& strength = *findTweak("BloomStrength");
    EXPECT_FALSE(sliderEnabled(ini, strength)); // bloom is off
    setChoice(ini, bloom, 2);
    EXPECT_TRUE(sliderEnabled(ini, strength));
    EXPECT_FLOAT_EQ(sliderValue(ini, strength), 0.7f); // Strong's preset
    setSlider(ini, strength, 1.2f);
    EXPECT_TRUE(sliderOverridden(ini, strength));
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).bloom, 1.2f);
    setSlider(ini, strength, 99.0f);
    EXPECT_FLOAT_EQ(sliderValue(ini, strength), strength.max); // clamped
    setChoice(ini, bloom, 1); // another preset: the override goes
    EXPECT_FALSE(sliderOverridden(ini, strength));
    EXPECT_FLOAT_EQ(sliderValue(ini, strength), 0.35f);
    setChoice(ini, bloom, 0);
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).bloom, 0.0f); // off whatever the slider says

    // Colour sliders work with the Original look too.
    const Tweak& brightness = *findTweak("Brightness");
    EXPECT_TRUE(sliderEnabled(ini, brightness));
    setSlider(ini, brightness, 1.2f);
    EXPECT_TRUE(postProcessFromTweaks(ini).active());
    EXPECT_EQ(sliderText(brightness, 1.2f), "1.20x");
    EXPECT_EQ(sliderText(*findTweak("AoRadius"), 0.8f), "0.8 m");
    EXPECT_EQ(sliderText(*findTweak("ShadowDarkness"), 0.45f), "45%");
}

// The Look: Original is nothing set, Remastered exactly its settings,
// anything else Custom; applying either sets that and only that.
TEST(Tweaks, LookPresets) {
    IniFile ini;
    EXPECT_EQ(lookValue(ini), 0);
    const Tweak& look = *findTweak("Look");
    setChoice(ini, look, 1);
    EXPECT_EQ(lookValue(ini), 1);
    EXPECT_EQ(tweakValue(ini, "Look"), 1);
    EXPECT_TRUE(tweakOn(ini, "SunShadows"));
    EXPECT_TRUE(postProcessFromTweaks(ini).active());
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).contrast, 1.06f);
    // Any change makes it Custom.
    setSlider(ini, *findTweak("BloomStrength"), 1.0f);
    EXPECT_EQ(lookValue(ini), 2);
    setChoice(ini, look, 1); // and back
    EXPECT_EQ(lookValue(ini), 1);
    setChoice(ini, look, 2); // Custom is not applied
    EXPECT_EQ(lookValue(ini), 1);
    setChoice(ini, look, 0);
    EXPECT_EQ(lookValue(ini), 0);
    EXPECT_FALSE(postProcessFromTweaks(ini).active());
    // Every setting of the Remastered look names a tweak and lies in range.
    for (const LookSetting& l : kRemasteredLook) {
        const Tweak* t = findTweak(l.key);
        ASSERT_NE(t, nullptr) << l.key;
        if (t->kind == TweakKind::Slider) {
            EXPECT_GE(l.value, t->min) << l.key;
            EXPECT_LE(l.value, t->max) << l.key;
        } else {
            EXPECT_LT(static_cast<int>(l.value), t->count) << l.key;
        }
    }
    // F9's original look draws without tweaks.
    EXPECT_FALSE(postProcessFromTweaks(drawnTweaks(ini, true)).active());
}
