#pragma once

// OpenMM2 night lighting (not in MM2): the lights that light the scene at
// night (render::SceneLight; mesh.frag's sceneLights). The car renderers add
// their headlights and sirens while a target is set (VehicleRenderer's
// glows, as MM2 places its ltLights); the race adds the street lamps
// (BangerSet::lampPositions) and picks the frame's lights.

#include "core/Math.h"
#include "render/Types.h"

#include <span>
#include <vector>

namespace mm2::game {

enum class SceneLightKind { Headlight, Lamp };

// While set, addSceneLight appends to `out` (null: lights are dropped).
void setSceneLightTarget(std::vector<render::SceneLight>* out);
// Multipliers of each kind's colour (the Tweaks page's strengths).
void setSceneLightScales(float headlights, float lamps);
void addSceneLight(SceneLightKind kind, render::SceneLight light);

// A street lamp's light at its glow (the bulb): a warm sodium point light.
render::SceneLight lampLight(const Vec3& bulb);

// Puts the lights that matter most for a camera at `eye` looking along
// `forward` into `frame` (at most render::kMaxSceneLights): those whose
// reach is closest, without those wholly behind the camera.
void selectSceneLights(std::span<const render::SceneLight> lights, const Vec3& eye, const Vec3& forward,
                       render::FrameConstants& frame);

} // namespace mm2::game
