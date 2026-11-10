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
    const Tweak& colour = *findTweak("ColorGrade");
    const Tweak& contrast = *findTweak("Contrast");
    EXPECT_TRUE(sliderEnabled(ini, contrast)); // colour sliders work with the original colour too
    setChoice(ini, colour, 3);
    EXPECT_FLOAT_EQ(sliderValue(ini, contrast), 1.15f); // Cinematic's preset
    setSlider(ini, contrast, 1.3f);
    EXPECT_TRUE(sliderOverridden(ini, contrast));
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).contrast, 1.3f);
    setSlider(ini, contrast, 99.0f);
    EXPECT_FLOAT_EQ(sliderValue(ini, contrast), contrast.max); // clamped
    setChoice(ini, colour, 2); // another preset: the override goes
    EXPECT_FALSE(sliderOverridden(ini, contrast));
    EXPECT_FLOAT_EQ(sliderValue(ini, contrast), 1.05f);

    const Tweak& brightness = *findTweak("Brightness");
    setSlider(ini, brightness, 1.2f);
    EXPECT_TRUE(postProcessFromTweaks(ini).active());
    EXPECT_EQ(sliderText(brightness, 1.2f), "1.20x");
    EXPECT_EQ(sliderText(*findTweak("Warmth"), -0.25f), "-0.25");
}

// Each level of a choice means its own value; off is none.
TEST(Tweaks, LevelsSetTheValues) {
    IniFile ini;
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).bloom, 0.0f);
    ini.setInt(kTweaksSection, "Bloom", 1);
    const float subtle = postProcessFromTweaks(ini).bloom;
    ini.setInt(kTweaksSection, "Bloom", 2);
    const float strong = postProcessFromTweaks(ini).bloom;
    EXPECT_GT(subtle, 0.0f);
    EXPECT_GT(strong, subtle);
    EXPECT_FLOAT_EQ(tweakLevel(ini, "Bloom", {0.0f, 1.0f, 2.0f}), 2.0f);
    EXPECT_FLOAT_EQ(tweakLevel(ini, "NoSuchTweak", {3.0f, 1.0f, 2.0f}), 3.0f);
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
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).contrast, 1.12f);   // Vivid, tuned
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).saturation, 1.25f); // Vivid's own
    EXPECT_FLOAT_EQ(postProcessFromTweaks(ini).bloom, 0.45f);      // Strong
    // Any change makes it Custom.
    setSlider(ini, *findTweak("Contrast"), 1.3f);
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
    // A slider stored at its preset's value changes nothing.
    applyLook(ini, 1);
    setSlider(ini, *findTweak("Contrast"), 1.12f);
    EXPECT_EQ(lookValue(ini), 1);
    // F9's original look draws without tweaks.
    EXPECT_FALSE(postProcessFromTweaks(drawnTweaks(ini, true)).active());
}
