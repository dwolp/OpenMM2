#pragma once

// OpenMM2 visual tweaks: optional enhancements that are not in MM2, each
// off by default so the default picture is the original's. They are listed
// on Graphics options -> TWEAKS and stored in the settings file's [Tweaks]
// section. A new tweak is a row here plus the code that reads it.
//
// Two kinds of row:
//  * a choice (Off / Subtle / Strong ...), stored as the chosen index; what
//    each level means is set where the tweak is read (tweakLevel), chosen
//    by eye;
//  * a slider that fine-tunes one value (only the colour's). Without a
//    stored override it takes its preset: the value its parent choice's
//    setting gives, and picking another choice clears the overrides under
//    it. A slider whose parent is off is dimmed.

#include "core/Ini.h"
#include "core/StringUtil.h"
#include "render/Types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace mm2::app {

// Preset: the Look row, which is not stored but derived from (and sets)
// all the others: Original, Remastered, or Custom when neither matches.
enum class TweakKind { Choice, Slider, Preset };
enum class TweakUnit { Times, Signed };

struct Tweak {
    TweakKind kind = TweakKind::Choice;
    std::string_view key;     // [Tweaks] key
    std::string_view section; // the page's group
    std::string_view label;
    std::string_view help;    // one line under the list while the row is current
    // Choices
    std::array<std::string_view, 4> choices{};
    int count = 2;    // used entries of `choices`
    int fallback = 0; // default: 0 = the original look
    bool look = true; // part of the Look (false: a control, which Original and Remastered leave alone)
    // Sliders
    std::string_view parent;               // the choice this fine-tunes (its index picks the preset)
    bool needsParent = true;               // dimmed and unused while the parent is 0
    std::array<float, 4> presets{};        // the value for each parent index
    float min = 0.0f, max = 1.0f, step = 0.05f;
    TweakUnit unit = TweakUnit::Times;
};

inline constexpr std::string_view kTweaksSection = "Tweaks";

constexpr Tweak choice(std::string_view key, std::string_view section, std::string_view label, std::string_view help,
                       std::array<std::string_view, 4> choices, int count) {
    Tweak t;
    t.kind = TweakKind::Choice;
    t.key = key;
    t.section = section;
    t.label = label;
    t.help = help;
    t.choices = choices;
    t.count = count;
    return t;
}
constexpr Tweak slider(std::string_view key, std::string_view parent, std::string_view label, std::string_view help,
                       std::array<float, 4> presets, float min, float max, float step, TweakUnit unit,
                       bool needsParent = true) {
    Tweak t;
    t.kind = TweakKind::Slider;
    t.key = key;
    t.parent = parent;
    t.label = label;
    t.help = help;
    t.presets = presets;
    t.min = min;
    t.max = max;
    t.step = step;
    t.unit = unit;
    t.needsParent = needsParent;
    return t;
}

// A choice that is not part of the Look (a control, such as the free camera).
constexpr Tweak control(std::string_view key, std::string_view section, std::string_view label,
                        std::string_view help, std::array<std::string_view, 4> choices, int count) {
    Tweak t = choice(key, section, label, help, choices, count);
    t.look = false;
    return t;
}

constexpr Tweak preset(std::string_view key, std::string_view section, std::string_view label, std::string_view help,
                       std::array<std::string_view, 4> choices, int count) {
    Tweak t = choice(key, section, label, help, choices, count);
    t.kind = TweakKind::Preset;
    return t;
}

// The REMASTERED look: the extras at the levels chosen by eye from
// side-by-side shots of morning, noon, evening, night, rain and a drive
// (everything not listed off). Vivid colour, high-resolution sun shadows out
// across the city, strong contact shading, glossy paint mirroring the live
// street, strong bloom and sun shafts. Not the grass: from the car its
// blades hardly show, and they cost the most in the parks.
struct LookSetting {
    std::string_view key;
    float value; // a choice's index, or a slider's value
};
inline constexpr std::array kRemasteredLook{
    LookSetting{"EnhancedCarLighting", 1}, LookSetting{"LiveReflections", 1}, LookSetting{"SunShadows", 2},
    LookSetting{"AmbientOcclusion", 2},    LookSetting{"NightLights", 1},     LookSetting{"Haze", 1},
    LookSetting{"ColorGrade", 1},      LookSetting{"Sharpen", 1},
    LookSetting{"Fxaa", 2},                LookSetting{"Vignette", 1},        LookSetting{"Bloom", 2},
    LookSetting{"WetRoads", 1},            LookSetting{"SunShafts", 2},       LookSetting{"MotionBlur", 2},
    LookSetting{"Water", 1},               LookSetting{"Clouds", 1},          LookSetting{"HeatShimmer", 1},
    LookSetting{"StreetDetail", 1},
};

// The RAY TRACED look: Remastered with the sun shadows and the
// reflections traced (where the GPU cannot, they are High).
inline constexpr std::array kRayTracedChanges{
    LookSetting{"SunShadows", 3},
    LookSetting{"LiveReflections", 3},
};
// A look's (1 Remastered, 2 Ray traced) value for `key`, if it sets one.
inline std::optional<float> lookSetting(int look, std::string_view key) {
    if (look == 2)
        for (const LookSetting& l : kRayTracedChanges)
            if (l.key == key)
                return l.value;
    for (const LookSetting& l : kRemasteredLook)
        if (l.key == key)
            return l.value;
    return std::nullopt;
}

// Sliders follow their parent: the page shows them under it, in its section.
inline constexpr std::array kTweaks{
    // --- Look --- (not stored: derived from everything below)
    preset("Look", "LOOK", "Look",
           "Enhanced turns every extra on; Enhanced RT also traces the shadows and reflections. "
           "F9 in a race compares with the original.",
           {"Original", "Enhanced", "Enhanced RT", "Custom"}, 4),
    // --- Lighting ---
    // Per-pixel car lighting with a highlight, Fresnel reflections and a
    // clear coat (game::VehicleRenderer::setEnhancedLighting, docs/rendering.md).
    choice("EnhancedCarLighting", "LIGHTING", "Car lighting",
           "Smooth per-pixel shading, glossy paint and angle-dependent reflections on the cars.",
           {"Original", "Enhanced"}, 2),
    // The world drawn around the player's car each frame, six views into a
    // reflection probe (game/ReflectionProbe.h): its paint and glass reflect
    // the real street. High draws each view at 512 instead of 256. Ray
    // traced: rays through the ray scene where the GPU can (High elsewhere).
    choice("LiveReflections", "LIGHTING", "Live reflections",
           "Your car's paint and windows reflect the real street, traffic and sky around it. Costly.",
           {"Off", "On", "High", "Ray traced"}, 4),
    // Real shadows from the sun (the key light): cars, props and buildings
    // (game/SunShadow.h). High: four cascades of 3072 instead of three of
    // 2048. Ray traced: rays towards the sun through the scene where the
    // GPU can (Vulkan ray queries; High elsewhere).
    choice("SunShadows", "LIGHTING", "Sun shadows",
           "Real shadows from the sun on cars, buildings, props and trees, out across the city. "
           "High is sharper and reaches farther; Ray traced is exact where the GPU can trace rays.",
           {"Off", "On", "High", "Ray traced"}, 4),
    // Contact shading where surfaces meet: under cars, in corners, along
    // curbs (screen-space ambient occlusion, ao.frag).
    choice("AmbientOcclusion", "LIGHTING", "Ambient occlusion",
           "Soft contact shading where things meet: under cars, along curbs, in corners.",
           {"Off", "Subtle", "Strong"}, 3),
    // Headlights, sirens and street lamps lighting the scene while the
    // cars' lights are on (game/SceneLights.h, mesh.frag's sceneLights);
    // at night the lit windows glow too (mesh.frag's windowLight).
    choice("NightLights", "LIGHTING", "Night lighting",
           "Headlights, sirens and street lamps light up the road, walls and cars after dark, and lit windows glow.",
           {"Off", "On"}, 2),
    // Big soft cloud shadows drifting over the city in the sun (mesh.frag's
    // cloudLight), with the sun shadows.
    choice("Clouds", "LIGHTING", "Cloud shadows",
           "Soft shadows of passing clouds drift over the city on sunny days. Needs sun shadows.",
           {"Off", "Few", "Many"}, 3),
    // Air between the camera and faraway buildings, brighter towards the
    // sun (mesh.frag's applyHaze).
    choice("Haze", "LIGHTING", "Distance haze",
           "A soft haze over faraway buildings and hills, glowing towards the sun.", {"Off", "Subtle", "Strong"}, 3),

    // --- Surfaces ---
    // Real blades on the city's grass textures near the camera (shell
    // layers, game::CityRenderer::drawGrass and mesh.frag's grass).
    choice("Grass", "SURFACES", "Grass", "Real blades of grass that sway in the wind, with lusher and drier patches.",
           {"Original", "Lush"}, 2),
    // The street geometry (roads, kerbs, dividers, facades) at full detail
    // however far (CityRenderer's DetailSettings::streetsFull).
    choice("StreetDetail", "SURFACES", "Street detail",
           "Roads, kerbs, dividers and building fronts at full detail however far away, so they do not pop in.",
           {"Original", "Full"}, 2),
    // The bay and ponds as water: moving waves, a mirror of the sky (or,
    // with ray traced reflections, of the city) and the sun's glint
    // (mesh.frag's waterSurface).
    choice("Water", "SURFACES", "Water",
           "Waves on the bay that mirror the sky and sparkle in the sun; with ray traced reflections the city and "
           "the bridges are reflected too.",
           {"Original", "Reflective"}, 2),

    // --- Image ---
    // Image filters on the composited scene (render::PostProcess,
    // shaders/composite.frag); the HUD and menus are not filtered.
    choice("ColorGrade", "IMAGE", "Colour", "A colour look for the whole picture; fine-tune it below.",
           {"Original", "Vivid", "Warm", "Cinematic"}, 4),
    slider("Brightness", "ColorGrade", "Brightness", "Overall brightness.", {1.0f, 1.0f, 1.0f, 1.0f}, 0.6f, 1.5f,
           0.02f, TweakUnit::Times, false),
    slider("Contrast", "ColorGrade", "Contrast", "Difference between darks and lights.", {1.0f, 1.12f, 1.05f, 1.15f},
           0.7f, 1.5f, 0.02f, TweakUnit::Times, false),
    slider("Saturation", "ColorGrade", "Saturation", "Colour intensity; 0 is black and white.",
           {1.0f, 1.25f, 1.10f, 0.85f}, 0.0f, 2.0f, 0.05f, TweakUnit::Times, false),
    slider("Warmth", "ColorGrade", "Warmth", "Cooler (blue) or warmer (orange).", {0.0f, 0.1f, 0.6f, 0.35f}, -1.0f,
           1.0f, 0.05f, TweakUnit::Signed, false),
    choice("Sharpen", "IMAGE", "Sharpening", "Crisper textures and edges.", {"Off", "Low", "High"}, 3),
    // FXAA (composite.frag) or temporal anti-aliasing with upscaling
    // (taa.frag: with Display's render scale below 1 the 3D view is drawn
    // smaller and resolved at the window's size).
    choice("Fxaa", "IMAGE", "Anti-aliasing",
           "Smooths jagged edges, also on fences and trees. TAA is smoother still, and with a render scale below "
           "100% it upscales.",
           {"Off", "FXAA", "TAA"}, 3),
    choice("Vignette", "IMAGE", "Vignette", "Darker corners, like a camera lens.", {"Off", "Subtle", "Strong"}, 3),

    // --- Effects ---
    // A glow around bright areas: lamps, headlights, the sky (bloom.frag).
    choice("Bloom", "EFFECTS", "Bloom", "A glow around bright lights: lamps, headlights, the sky.",
           {"Off", "Subtle", "Strong"}, 3),
    // Reflections on wet ground and puddles (ssr.frag): only when it rains,
    // or always.
    choice("WetRoads", "EFFECTS", "Wet roads", "Reflections on wet streets and puddles, in rain or always.",
           {"Off", "In rain", "Always"}, 3),
    // The air over hot roads wavering on a sunny noon (ssr.frag's
    // heatShimmer).
    choice("HeatShimmer", "EFFECTS", "Heat shimmer",
           "On a sunny noon the air over the road ahead wavers, and far off the road mirrors the sky.",
           {"Off", "On"}, 2),
    // Light streaming from the sun past buildings and trees, and its glare
    // (shafts.frag).
    choice("SunShafts", "EFFECTS", "Sun shafts", "Light streaming past buildings and trees, and the sun's glare.",
           {"Off", "Subtle", "Strong"}, 3),
    // Camera motion blur for a sense of speed (motionblur.frag).
    choice("MotionBlur", "EFFECTS", "Motion blur", "Blurs the world around the car at speed.",
           {"Off", "Subtle", "Strong"}, 3),

    // --- Camera --- (not part of the Look)
    // The chase cameras swung round the car by the right stick, or the mouse
    // with its right button held (RaceScreen::applyFreeLook).
    control("FreeLook", "CAMERA", "Free camera",
            "Right stick, or the mouse with its right button held, swings the camera round the car.",
            {"Off", "On"}, 2),
};

inline const Tweak* findTweak(std::string_view key) {
    for (const Tweak& t : kTweaks)
        if (t.key == key)
            return &t;
    return nullptr;
}

inline int lookValue(const IniFile& ini);

// The chosen index of a choice (the Look: lookValue), clamped to its
// choices; unknown keys are 0.
inline int tweakValue(const IniFile& ini, std::string_view key) {
    const Tweak* t = findTweak(key);
    if (!t || t->kind == TweakKind::Slider)
        return 0;
    if (t->kind == TweakKind::Preset)
        return lookValue(ini);
    return static_cast<int>(std::clamp<long long>(ini.getInt(kTweaksSection, key, t->fallback), 0, t->count - 1));
}
inline bool tweakOn(const IniFile& ini, std::string_view key) { return tweakValue(ini, key) != 0; }
// What a choice's level means: `values` by its chosen index.
inline float tweakLevel(const IniFile& ini, std::string_view key, const std::array<float, 4>& values) {
    return values[static_cast<std::size_t>(std::clamp(tweakValue(ini, key), 0, 3))];
}

// A slider's value: its override clamped to its range, else its preset.
inline float sliderPreset(const IniFile& ini, const Tweak& t) {
    const int parent = std::clamp(tweakValue(ini, t.parent), 0, 3);
    return t.presets[static_cast<std::size_t>(parent)];
}
inline float sliderValue(const IniFile& ini, const Tweak& t) {
    if (auto v = ini.get(kTweaksSection, t.key))
        if (auto f = str::parseDouble(*v))
            return std::clamp(static_cast<float>(*f), t.min, t.max);
    return sliderPreset(ini, t);
}
inline float sliderValue(const IniFile& ini, std::string_view key) {
    const Tweak* t = findTweak(key);
    return t && t->kind == TweakKind::Slider ? sliderValue(ini, *t) : 0.0f;
}
inline bool sliderOverridden(const IniFile& ini, const Tweak& t) { return ini.get(kTweaksSection, t.key).has_value(); }
inline bool sliderEnabled(const IniFile& ini, const Tweak& t) { return !t.needsParent || tweakOn(ini, t.parent); }

inline void applyLook(IniFile& ini, int look);

// Sets a choice and clears the overrides of the sliders under it, so the
// new preset's values apply. The Look applies its look instead.
inline void setChoice(IniFile& ini, const Tweak& t, int index) {
    if (t.kind == TweakKind::Preset) {
        applyLook(ini, index);
        return;
    }
    ini.setInt(kTweaksSection, t.key, std::clamp(index, 0, t.count - 1));
    for (const Tweak& s : kTweaks)
        if (s.kind == TweakKind::Slider && s.parent == t.key)
            ini.remove(kTweaksSection, s.key);
}
inline void setSlider(IniFile& ini, const Tweak& t, float value) {
    value = std::clamp(value, t.min, t.max);
    char text[32];
    std::snprintf(text, sizeof text, "%g", static_cast<double>(value));
    ini.set(kTweaksSection, t.key, text);
}
// A slider's value as the page shows it.
inline std::string sliderText(const Tweak& t, float v) {
    char text[32];
    switch (t.unit) {
    case TweakUnit::Times: std::snprintf(text, sizeof text, "%.2fx", static_cast<double>(v)); break;
    case TweakUnit::Signed: std::snprintf(text, sizeof text, "%+.2f", static_cast<double>(v)); break;
    }
    return text;
}

// The image filters the tweaks choose; all off gives the default (no
// filtering). The levels were chosen by eye.
inline render::PostProcess postProcessFromTweaks(const IniFile& ini) {
    render::PostProcess p;
    p.exposure = sliderValue(ini, "Brightness");
    p.contrast = sliderValue(ini, "Contrast");
    p.saturation = sliderValue(ini, "Saturation");
    p.warmth = sliderValue(ini, "Warmth");
    p.sharpen = tweakLevel(ini, "Sharpen", {0.0f, 0.4f, 0.8f});
    p.fxaa = tweakValue(ini, "Fxaa") == 1;
    p.taa = tweakValue(ini, "Fxaa") == 2;
    p.vignette = tweakLevel(ini, "Vignette", {0.0f, 0.25f, 0.5f});
    p.bloom = tweakLevel(ini, "Bloom", {0.0f, 0.35f, 0.7f});
    p.bloomThreshold = 0.65f;
    p.ao = tweakLevel(ini, "AmbientOcclusion", {0.0f, 0.5f, 0.8f});
    p.aoRadius = 0.8f;
    // "In rain" is decided by the race (it knows the weather); here both wet.
    p.wetness = tweakOn(ini, "WetRoads") ? 1.0f : 0.0f;
    // Only on a clear noon, which the race decides; here on.
    p.heat = tweakOn(ini, "HeatShimmer") ? 1.0f : 0.0f;
    p.shafts = tweakLevel(ini, "SunShafts", {0.0f, 0.6f, 1.0f});
    p.motionBlur = tweakLevel(ini, "MotionBlur", {0.0f, 0.5f, 1.0f});
    return p;
}

// The Look: 0 Original (nothing set), 1 Remastered (exactly
// kRemasteredLook), 2 Ray traced (that with kRayTracedChanges), 3 Custom
// (anything else).
inline int lookValue(const IniFile& ini) {
    auto stored = [&](const Tweak& t) { return ini.get(kTweaksSection, t.key).has_value(); };
    // A slider stored at its preset's value changes nothing (an older
    // Remastered stored the colour's, which are now Vivid's own).
    auto overridden = [&](const Tweak& t) {
        return stored(t) && std::abs(sliderValue(ini, t) - sliderPreset(ini, t)) >= 1e-4f;
    };
    bool original = true;
    for (const Tweak& t : kTweaks)
        if (!t.look)
            continue;
        else if (t.kind == TweakKind::Choice ? tweakValue(ini, t.key) != t.fallback
                                        : t.kind == TweakKind::Slider && overridden(t))
            original = false;
    if (original)
        return 0;
    // Exactly one of the looks' settings, and nothing else.
    auto matches = [&](int look) {
        for (const Tweak& t : kTweaks) {
            if (t.kind == TweakKind::Preset || !t.look)
                continue;
            const auto want = lookSetting(look, t.key);
            if (t.kind == TweakKind::Choice) {
                if (tweakValue(ini, t.key) != (want ? static_cast<int>(*want) : t.fallback))
                    return false;
            } else if (want ? std::abs(sliderValue(ini, t) - *want) >= 1e-4f : overridden(t)) {
                return false;
            }
        }
        return true;
    };
    if (matches(1))
        return 1;
    if (matches(2))
        return 2;
    return 3;
}
// Original clears every tweak of the Look; Remastered and Ray traced set
// theirs (and nothing else); Custom (3) changes nothing. Controls are kept.
inline void applyLook(IniFile& ini, int look) {
    if (look < 0 || look > 2)
        return;
    for (const Tweak& t : kTweaks)
        if (t.look)
            ini.remove(kTweaksSection, t.key);
    if (look == 0)
        return;
    for (const LookSetting& l : kRemasteredLook)
        if (const Tweak* t = findTweak(l.key)) {
            const float value = *lookSetting(look, l.key);
            if (t->kind == TweakKind::Choice)
                ini.setInt(kTweaksSection, t->key, static_cast<int>(value));
            else
                setSlider(ini, *t, value);
        }
}

// The settings the game draws with: the tweaks as set, or none while the
// race's F9 shows the original look (Context::originalLook).
inline const IniFile& drawnTweaks(const IniFile& settings, bool originalLook) {
    static const IniFile none;
    return originalLook ? none : settings;
}

// Copies every tweak of `from` into `to` (a key `from` lacks is removed).
inline void copyTweaks(const IniFile& from, IniFile& to) {
    for (const Tweak& t : kTweaks) {
        if (auto v = from.get(kTweaksSection, t.key))
            to.set(kTweaksSection, t.key, *v);
        else
            to.remove(kTweaksSection, t.key);
    }
}

} // namespace mm2::app
