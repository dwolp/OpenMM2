#pragma once

// OpenMM2 sun shadows (not in MM2): where the shadow map looks from. The
// game's key light (the time of day's .ltNN light 0) is the sun; the map
// is an orthographic view along it, covering a square around a point ahead
// of the camera. See docs/rendering.md.

#include "core/Math.h"
#include "render/Types.h"

#include <cstdint>

namespace mm2::game {

struct SunShadowView {
    Mat44 view;
    Mat44 proj;
    Vec3 eye;          // the shadow camera's position (for the frame constants)
    float texel = 0;   // world size of one shadow-map texel
    Mat44 viewProj() const { return view * proj; }
};

struct SunShadowSettings {
    std::uint32_t size = 2048; // texels per side
    float radius = 70.0f;      // half the covered square's side, metres
    float ahead = 0.55f;       // the square's centre, radii ahead of the camera
    float depth = 600.0f;      // metres along the light, centred on the square
};

// `lightDirection`: the direction the light travels (render::DirectionalLight).
// The centre snaps to whole texels in the light's plane, so shadows do not
// shimmer as the camera moves. nullopt-free: a light pointing up (below the
// horizon) still gives a view; callers decide whether to draw it.
SunShadowView sunShadowView(const Vec3& lightDirection, const Mat34& camera, const SunShadowSettings& settings);

// How dark sun shadows are in `frame`'s light, before the tweak's strength:
// the key light's brightness against the ambient (none when the key light
// is below the horizon or black).
float sunShadowDarkness(const render::FrameConstants& frame);

} // namespace mm2::game
