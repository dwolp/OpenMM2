#include "game/SceneLights.h"

#include <algorithm>
#include <cmath>

namespace mm2::game {
namespace {
std::vector<render::SceneLight>* g_target = nullptr;
float g_headlightScale = 1.0f;
float g_lampScale = 1.0f;
} // namespace

void setSceneLightTarget(std::vector<render::SceneLight>* out) { g_target = out; }

void setSceneLightScales(float headlights, float lamps) {
    g_headlightScale = headlights;
    g_lampScale = lamps;
}

void addSceneLight(SceneLightKind kind, render::SceneLight light) {
    if (!g_target)
        return;
    const float scale = kind == SceneLightKind::Lamp ? g_lampScale : g_headlightScale;
    if (scale <= 0.0f)
        return;
    light.color = light.color * scale;
    g_target->push_back(light);
}

render::SceneLight lampLight(const Vec3& bulb) {
    // Chosen by eye: high-pressure sodium orange, a pool about 20 m across
    // under a lamp 7-12 m up.
    render::SceneLight l;
    l.position = bulb;
    l.range = 24.0f;
    l.color = Vec3{1.0f, 0.68f, 0.38f} * (3.0f * g_lampScale);
    return l;
}

void selectSceneLights(std::span<const render::SceneLight> lights, const Vec3& eye, const Vec3& forward,
                       render::FrameConstants& frame) {
    struct Scored {
        float score;
        const render::SceneLight* light;
    };
    std::vector<Scored> scored;
    scored.reserve(lights.size());
    for (const render::SceneLight& l : lights) {
        const Vec3 to = l.position - eye;
        // Wholly behind the camera: nothing of it can be in view.
        if (to.dot(forward) < -l.range)
            continue;
        // Near lights first; a light's reach counts only partly, so the
        // long headlight beams of far traffic do not crowd out the lamps.
        const float d = std::sqrt(to.dot(to));
        scored.push_back({std::max(d - l.range, 0.0f) + 0.5f * d, &l});
    }
    const std::size_t n = std::min(scored.size(), render::kMaxSceneLights);
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(n), scored.end(),
                      [](const Scored& a, const Scored& b) { return a.score < b.score; });
    for (std::size_t i = 0; i < n; ++i)
        frame.sceneLights[i] = *scored[i].light;
    frame.sceneLightCount = static_cast<std::uint32_t>(n);
}

} // namespace mm2::game
