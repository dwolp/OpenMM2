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
// Multiplies the headlights' and sirens' colour (dimmed in daylight); 0
// drops them.
void setHeadlightScale(float scale);
void addSceneLight(SceneLightKind kind, render::SceneLight light);

// A street lamp's light at its glow (the bulb): a warm sodium point light.
render::SceneLight lampLight(const Vec3& bulb);
// The same for a lamp gathered within `reach` of `eye`: faded out over the
// last 30% of the reach, so lamps come in smoothly instead of popping on.
render::SceneLight lampLight(const Vec3& bulb, const Vec3& eye, float reach);

// Puts the lights that matter most for a camera at `eye` looking along
// `forward` into `frame` (at most render::kMaxSceneLights): those whose
// reach is closest, without those wholly behind the camera.
void selectSceneLights(std::span<const render::SceneLight> lights, const Vec3& eye, const Vec3& forward,
                       render::FrameConstants& frame);

} // namespace mm2::game
