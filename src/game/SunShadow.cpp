#include "game/SunShadow.h"

#include <algorithm>
#include <cmath>

namespace mm2::game {

SunShadowView sunShadowView(const Vec3& lightDirection, const Mat34& camera, const SunShadowSettings& settings) {
    const float r = settings.radius;
    const float texel = 2.0f * r / static_cast<float>(std::max(settings.size, 1u));

    // The square's centre: the camera (turning it moves nothing).
    const Vec3 centre = camera.m3;

    // The light camera looks along the light (down its -Z): Z = -L.
    Vec3 l = lightDirection;
    const float ll = std::sqrt(l.dot(l));
    l = ll > 1e-6f ? l * (1.0f / ll) : Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 z = l * -1.0f;
    const Vec3 up = std::abs(z.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 x = up.cross(z).normalized();
    const Vec3 y = z.cross(x);

    // Snap the centre to whole texels across the light (and along it, so a
    // view that has not moved across it is the same view), then back off
    // along it by half the depth range.
    const float cx = centre.dot(x), cy = centre.dot(y), cz = centre.dot(z);
    const float step = texel * std::max(settings.snap, 1.0f);
    const float sx = std::floor(cx / step) * step, sy = std::floor(cy / step) * step;
    const float sz = std::floor(cz / step) * step;
    // From the snapped coordinates alone (the basis is orthonormal), so the
    // same steps give bit for bit the same view.
    const Vec3 snapped = x * sx + y * sy + z * sz;
    const Vec3 eye = snapped - l * (settings.depth * 0.5f);

    Mat34 t;
    t.m0 = x;
    t.m1 = y;
    t.m2 = z;
    t.m3 = eye;
    SunShadowView v;
    v.view = Mat44::fromMat34(t.fastInverse());
    v.proj = Mat44::orthographic(-r, r, -r, r, 0.0f, settings.depth, true);
    v.eye = eye;
    v.texel = texel;
    v.depth = settings.depth;
    return v;
}

SunShadowCascades sunShadowCascades(const Vec3& lightDirection, const Mat34& camera,
                                    const SunShadowCascadeSettings& settings) {
    SunShadowCascades out;
    out.cellSize = settings.cellSize;
    out.count = std::min<std::uint32_t>(settings.count, render::FrameConstants::kShadowCascades);
    float radius = settings.radius;
    for (std::uint32_t i = 0; i < out.count; ++i) {
        SunShadowSettings s;
        s.size = settings.cellSize;
        s.radius = radius;
        s.snap = i >= settings.coarseFrom ? static_cast<float>(settings.cellSize) / 16.0f : 1.0f;
        s.depth = std::max(600.0f, 2.0f * radius + 400.0f);
        out.views[i] = sunShadowView(lightDirection, camera, s);
        radius *= settings.growth;
    }
    return out;
}

render::Rect sunShadowCell(std::size_t i, std::uint32_t cellSize) {
    return {static_cast<std::int32_t>((i % 2) * cellSize), static_cast<std::int32_t>((i / 2) * cellSize), cellSize,
            cellSize};
}

float sunShadowDarkness(const render::FrameConstants& frame) {
    const render::DirectionalLight& key = frame.lights[0];
    // The light must come from above the horizon (travel downwards).
    const Vec3 d = key.direction;
    const float len = std::sqrt(d.dot(d));
    if (len < 1e-6f || d.y / len > -0.05f)
        return 0.0f;
    const auto luma = [](const Vec3& c) { return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z; };
    const float sun = luma(key.color);
    const float ambient = luma(frame.ambient);
    // The share of the light the sun gives, so dusk and night shadows are
    // fainter than noon's; the fills stay (they light the shadowed side).
    const float share = sun / std::max(sun + ambient + 0.5f * (luma(frame.lights[1].color) + luma(frame.lights[2].color)), 1e-3f);
    return std::clamp(share * 1.6f, 0.0f, 1.0f);
}

} // namespace mm2::game
