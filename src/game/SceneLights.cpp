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
    l.color = Vec3{1.0f, 0.68f, 0.38f} * (2.4f * g_lampScale);
    return l;
}

render::SceneLight lampLight(const Vec3& bulb, const Vec3& eye, float reach) {
    render::SceneLight l = lampLight(bulb);
    const float d = std::sqrt((bulb - eye).dot(bulb - eye));
    const float t = std::clamp((d - 0.7f * reach) / (0.3f * reach), 0.0f, 1.0f);
    l.color = l.color * (1.0f - t * t * (3.0f - 2.0f * t));
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
    // One past the budget too: the first light left out.
    const std::size_t sorted = std::min(scored.size(), n + 1);
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(sorted), scored.end(),
                      [](const Scored& a, const Scored& b) { return a.score < b.score; });
    // With more lights than the budget, the kept ones fade out as their
    // score nears the first one left out, so a light dropped as the
    // camera moves has already faded rather than popping off.
    const float cut = sorted > n ? scored[n].score : 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        render::SceneLight l = *scored[i].light;
        if (cut > 0.0f) {
            const float t = std::clamp((scored[i].score - 0.7f * cut) / (0.3f * cut), 0.0f, 1.0f);
            l.color = l.color * (1.0f - t * t * (3.0f - 2.0f * t));
        }
        frame.sceneLights[i] = l;
    }
    frame.sceneLightCount = static_cast<std::uint32_t>(n);
}

} // namespace mm2::game
