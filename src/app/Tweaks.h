#pragma once

// OpenMM2 visual tweaks: optional enhancements that are not in MM2, each
// off by default so the default picture is the original's. They are listed
// on Graphics options -> TWEAKS and stored in the settings file's [Tweaks]
// section as the index of the chosen value. A new tweak is a row here plus
// the code that reads it.

#include "core/Ini.h"

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
};

// The chosen value of `key`, clamped to its choices; unknown keys are 0.
inline int tweakValue(const IniFile& ini, std::string_view key) {
    for (const Tweak& t : kTweaks)
        if (t.key == key)
            return static_cast<int>(std::clamp<long long>(ini.getInt(kTweaksSection, key, t.fallback), 0, t.count - 1));
    return 0;
}
inline bool tweakOn(const IniFile& ini, std::string_view key) { return tweakValue(ini, key) != 0; }

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
