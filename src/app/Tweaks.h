#pragma once

// OpenMM2 visual tweaks: optional enhancements that are not in MM2, each
// off by default so the default picture is the original's. They are listed
// on Graphics options -> TWEAKS and stored in the settings file's [Tweaks]
// section as the index of the chosen value. A new tweak is a row here plus
// the code that reads it.

#include "core/Ini.h"
#include "render/Types.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace mm2::app {

struct Tweak {
    std::string_view key;   // [Tweaks] key
    std::string_view label; // the TWEAKS page's label
    std::array<std::string_view, 4> choices{};
    int count = 2;          // used entries of `choices`
    int fallback = 0;       // default: 0 = the original look
};

inline constexpr std::string_view kTweaksSection = "Tweaks";

inline constexpr std::array kTweaks{
    // Per-pixel car lighting with a highlight and Fresnel reflections
    // (game::VehicleRenderer::setEnhancedLighting, docs/rendering.md).
    Tweak{"EnhancedCarLighting", "CAR LIGHTING", {"Original", "Enhanced"}, 2, 0},
    // Image filters on the composited scene (render::PostProcess,
    // shaders/composite.frag); the HUD and menus are not filtered.
    Tweak{"ColorGrade", "COLOUR", {"Original", "Vivid", "Warm", "Cinematic"}, 4, 0},
    Tweak{"Sharpen", "SHARPENING", {"Off", "Low", "High"}, 3, 0},
    Tweak{"Fxaa", "FXAA", {"Off", "On"}, 2, 0},
    Tweak{"Vignette", "VIGNETTE", {"Off", "Subtle", "Strong"}, 3, 0},
    // A glow around bright areas: lamps, headlights, the sky (bloom.frag).
    Tweak{"Bloom", "BLOOM", {"Off", "Subtle", "Strong"}, 3, 0},
    // Contact shading where surfaces meet: under cars, in corners, along
    // curbs (screen-space ambient occlusion, ao.frag).
    Tweak{"AmbientOcclusion", "AMBIENT OCCLUSION", {"Off", "Subtle", "Strong"}, 3, 0},
    // Real shadows from the sun (the key light): cars, props and buildings
    // (game/SunShadow.h). High uses a 4096 shadow map instead of 2048.
    Tweak{"SunShadows", "SUN SHADOWS", {"Off", "On", "High"}, 3, 0},
    // Reflections on wet ground and puddles (ssr.frag): only when it rains,
    // or always.
    Tweak{"WetRoads", "WET ROADS", {"Off", "In rain", "Always"}, 3, 0},
};

// The chosen value of `key`, clamped to its choices; unknown keys are 0.
inline int tweakValue(const IniFile& ini, std::string_view key) {
    for (const Tweak& t : kTweaks)
        if (t.key == key)
            return static_cast<int>(std::clamp<long long>(ini.getInt(kTweaksSection, key, t.fallback), 0, t.count - 1));
    return 0;
}
inline bool tweakOn(const IniFile& ini, std::string_view key) { return tweakValue(ini, key) != 0; }

// The image filters the tweaks choose; all off gives the default (no
// filtering). The values were chosen by eye.
inline render::PostProcess postProcessFromTweaks(const IniFile& ini) {
    render::PostProcess p;
    switch (tweakValue(ini, "ColorGrade")) {
    case 1: p.contrast = 1.10f, p.saturation = 1.25f; break;                     // Vivid
    case 2: p.contrast = 1.05f, p.saturation = 1.10f, p.warmth = 0.6f; break;    // Warm
    case 3: p.contrast = 1.15f, p.saturation = 0.85f, p.warmth = 0.35f; break;   // Cinematic
    default: break;
    }
    const float sharpen[] = {0.0f, 0.4f, 0.8f};
    p.sharpen = sharpen[tweakValue(ini, "Sharpen")];
    p.fxaa = tweakOn(ini, "Fxaa");
    const float vignette[] = {0.0f, 0.25f, 0.5f};
    p.vignette = vignette[tweakValue(ini, "Vignette")];
    const float bloom[] = {0.0f, 0.35f, 0.7f};
    p.bloom = bloom[tweakValue(ini, "Bloom")];
    const float ao[] = {0.0f, 0.6f, 1.0f};
    p.ao = ao[tweakValue(ini, "AmbientOcclusion")];
    // "In rain" is decided by the race (it knows the weather); here both wet.
    p.wetness = tweakValue(ini, "WetRoads") > 0 ? 1.0f : 0.0f;
    return p;
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
