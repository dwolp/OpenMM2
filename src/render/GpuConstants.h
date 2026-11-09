#pragma once

// std140 mirrors of the shader constant blocks (see shaders/common.glsl).
// Shared by the Vulkan and OpenGL backends.

#include "render/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mm2::render::detail {

struct alignas(16) GpuFrameConstants {
    float view[16];
    float proj[16];
    float viewProj[16];
    float cameraPos[4];
    float fogColor[4];
    float fogParams[4]; // start, end, density, mode
    float ambient[4];
    float lightDir[3][4];
    float lightColor[3][4];
    float shadowViewProj[16];
    float shadowParams[4]; // strength, texel (world), size (texels), softness
    float surface[4];      // detail, relief, smooth filtering, distance
    float ground[4];       // variation, grass height, grass distance, time
    float haze[4];         // density, height falloff, sun glow, most
    float lightInfo[4];    // scene light count, unused
    float lightPos[kMaxSceneLights][4];   // position, range
    float lightSpot[kMaxSceneLights][4];  // direction, cos outer (< -1: point)
    float lightTint[kMaxSceneLights][4];  // colour, cos inner
};
static_assert(sizeof(GpuFrameConstants) == 496 + 3 * 16 * kMaxSceneLights);

// Push constants on Vulkan (<= 128 bytes), a uniform block on OpenGL.
struct alignas(16) GpuDrawConstants {
    float world[16];
    float color[4];
    float alphaRef;
    std::uint32_t flags;
    float specular;
    float specularPower;
    float emissive[4];
};
static_assert(sizeof(GpuDrawConstants) == 112);

// The composite pass's filters (shaders/composite.frag), in the draw
// constants' slot: push constants on Vulkan, the DrawBlock on OpenGL.
struct alignas(16) GpuPostConstants {
    float grade[4];  // contrast, saturation, warmth, vignette
    float detail[4]; // sharpen, fxaa (0/1), active (0/1), bloom intensity
    float extra[4];  // exposure, unfiltered rectangles (count), unused
    float rects[2][4]; // unfiltered rectangles in the scene's uv (x0, y0, x1, y1)
};
static_assert(sizeof(GpuPostConstants) <= sizeof(GpuDrawConstants));

// The unfiltered rectangles (Device::addUnfilteredRect) in the uv the
// composite samples the scene with: rows top first (Vulkan) or bottom first
// (OpenGL).
inline void setUnfilteredRects(GpuPostConstants& c, const std::vector<Rect>& rects, Extent2D scene, bool bottomUp) {
    const float w = static_cast<float>(std::max(scene.width, 1u)), h = static_cast<float>(std::max(scene.height, 1u));
    int n = 0;
    for (const Rect& r : rects) {
        if (n == 2)
            break;
        const float x0 = static_cast<float>(r.x) / w, x1 = static_cast<float>(r.x + static_cast<int>(r.width)) / w;
        float y0 = static_cast<float>(r.y) / h, y1 = static_cast<float>(r.y + static_cast<int>(r.height)) / h;
        if (bottomUp) {
            const float t = 1.0f - y1;
            y1 = 1.0f - y0;
            y0 = t;
        }
        c.rects[n][0] = x0;
        c.rects[n][1] = y0;
        c.rects[n][2] = x1;
        c.rects[n][3] = y1;
        ++n;
    }
    c.extra[1] = static_cast<float>(n);
}

inline GpuPostConstants toGpu(const PostProcess& p) {
    return {{p.contrast, p.saturation, p.warmth, p.vignette},
            {p.sharpen, p.fxaa ? 1.0f : 0.0f, p.active() ? 1.0f : 0.0f, p.bloom},
            {p.exposure, 0.0f, 0.0f, 0.0f},
            {}};
}

// One pass of the bloom chain (shaders/bloom.frag).
struct alignas(16) GpuBloomConstants {
    float params[4]; // mode (0 bright pass + down, 1 down, 2 up), threshold, unused, unused
    float texel[4];  // 1 / size of the source (uTexture0), 1 / size of the added level (uTexture1)
};
static_assert(sizeof(GpuBloomConstants) <= sizeof(GpuDrawConstants));

// The other post passes' constants (depthcopy, ao, aoblur .frag).
struct alignas(16) GpuPassConstants {
    float a[4];
    float b[4];
};
static_assert(sizeof(GpuPassConstants) <= sizeof(GpuDrawConstants));

// Ambient occlusion's passes for a scene target of w x h drawn with `proj`
// (Mat44::perspective, 0..1 depth): distance t = B / (d + A), the far
// plane where d = 1.
inline float projectionFar(const Mat44& proj) { return proj.m[3][2] / (1.0f + proj.m[2][2]); }
inline GpuPassConstants depthCopyConstants(const Mat44& proj) {
    return {{proj.m[2][2], proj.m[3][2], projectionFar(proj), 0.0f}, {}};
}
inline GpuPassConstants aoConstants(const Mat44& proj, const PostProcess& p, std::uint32_t w, std::uint32_t h) {
    return {{proj.m[0][0], proj.m[1][1], p.aoRadius, 0.0f},
            {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), projectionFar(proj), p.aoFade}};
}
inline GpuPassConstants aoBlurConstants(const PostProcess& p, std::uint32_t w, std::uint32_t h) {
    return {{p.ao, 0.0f, 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h)}, {}};
}

// Wet roads' reflections (ssr.frag).
struct alignas(16) GpuSsrConstants {
    float params[4]; // proj[0][0], proj[1][1], wetness, time
    float up[4];     // the world's up in view space, far plane
    float cam[3][4]; // the camera's axis i (xyz) and position component i (w)
};
static_assert(sizeof(GpuSsrConstants) <= sizeof(GpuDrawConstants));

inline GpuSsrConstants ssrConstants(const Mat44& proj, const Mat34& camera, const PostProcess& p) {
    GpuSsrConstants c{};
    c.params[0] = proj.m[0][0];
    c.params[1] = proj.m[1][1];
    c.params[2] = p.wetness;
    c.params[3] = p.time;
    c.up[0] = camera.m0.y;
    c.up[1] = camera.m1.y;
    c.up[2] = camera.m2.y;
    c.up[3] = projectionFar(proj);
    const Vec3* axes[3] = {&camera.m0, &camera.m1, &camera.m2};
    const float pos[3] = {camera.m3.x, camera.m3.y, camera.m3.z};
    for (int i = 0; i < 3; ++i) {
        c.cam[i][0] = axes[i]->x;
        c.cam[i][1] = axes[i]->y;
        c.cam[i][2] = axes[i]->z;
        c.cam[i][3] = pos[i];
    }
    return c;
}

// Sun shafts and glare (shafts.frag): the sun's screen position from the
// direction towards it in view space; its weight fades as it leaves the
// screen and is 0 behind the camera.
inline GpuPassConstants shaftsConstants(const Mat44& proj, const Mat34& camera, const Vec3& lightDirection,
                                        const Vec3& color, const PostProcess& p) {
    const Vec3 toSun = lightDirection * -1.0f;
    const Vec3 v{toSun.dot(camera.m0), toSun.dot(camera.m1), toSun.dot(camera.m2)};
    GpuPassConstants c{};
    c.b[0] = color.x;
    c.b[1] = color.y;
    c.b[2] = color.z;
    c.b[3] = projectionFar(proj);
    if (v.z >= -0.05f) // behind the camera, or at its side
        return c;
    const float x = v.x * proj.m[0][0] / -v.z, y = v.y * proj.m[1][1] / -v.z;
    const float out = std::max(std::abs(x), std::abs(y));
    c.a[0] = x;
    c.a[1] = y;
    c.a[2] = out <= 1.0f ? 1.0f : std::max(0.0f, 1.0f - (out - 1.0f) / 0.6f);
    c.a[3] = p.shafts;
    return c;
}

// Camera motion blur (motionblur.frag): this frame's view space to the
// previous frame's clip space, and the exposure scaled to the frame time.
struct alignas(16) GpuMotionBlurConstants {
    float toPrevious[16];
    float params[4]; // proj[0][0], proj[1][1], far, exposure scale
    float limits[4]; // longest blur (uv), sharp within (m), fully blurred from (m), unused
};
static_assert(sizeof(GpuMotionBlurConstants) <= sizeof(GpuDrawConstants));

inline GpuMotionBlurConstants motionBlurConstants(const Mat44& proj, const Mat34& camera, const Mat44& prevProj,
                                                  const Mat34& prevCamera, const PostProcess& p, float frameTime) {
    GpuMotionBlurConstants c{};
    const Mat44 toPrevious = Mat44::fromMat34(camera) * Mat44::fromMat34(prevCamera.fastInverse()) * prevProj;
    std::memcpy(c.toPrevious, toPrevious.m, sizeof(c.toPrevious)); // as storeMatrix (below)
    c.params[0] = proj.m[0][0];
    c.params[1] = proj.m[1][1];
    c.params[2] = projectionFar(proj);
    // The motion of one 1/60 s exposure x strength, whatever the frame rate.
    c.params[3] = frameTime > 1e-5f ? p.motionBlur * (1.0f / 60.0f) / frameTime : 0.0f;
    // No blur across a cut (another camera view, a reset, a new race) or a
    // long stall: the "motion" there is a jump, not movement.
    const Vec3 moved = camera.m3 - prevCamera.m3;
    if (moved.dot(moved) > 8.0f * 8.0f || camera.m2.dot(prevCamera.m2) < 0.9f || frameTime > 0.25f)
        c.params[3] = 0.0f;
    c.limits[0] = 0.05f;
    c.limits[1] = 6.0f;
    c.limits[2] = 12.0f;
    return c;
}

// The bloom chain's levels: level 0 is half the scene target, each next
// one half again (at least 1 pixel).
inline constexpr int kBloomLevels = 5;

// Matrices are stored row-major exactly as Mat44 holds them. GLSL reads them
// column-major, i.e. transposed, so `M * v` in a shader equals the engine's
// row-vector `v * M`.
inline void storeMatrix(float* dst, const Mat44& m) { std::memcpy(dst, m.m, sizeof(float) * 16); }

inline GpuFrameConstants toGpu(const FrameConstants& fc) {
    GpuFrameConstants g{};
    storeMatrix(g.view, fc.view);
    storeMatrix(g.proj, fc.proj);
    storeMatrix(g.viewProj, fc.view * fc.proj);
    g.cameraPos[0] = fc.cameraPosition.x;
    g.cameraPos[1] = fc.cameraPosition.y;
    g.cameraPos[2] = fc.cameraPosition.z;
    g.cameraPos[3] = 1.0f;
    g.fogColor[0] = fc.fogColor.x;
    g.fogColor[1] = fc.fogColor.y;
    g.fogColor[2] = fc.fogColor.z;
    g.fogColor[3] = 1.0f;
    g.fogParams[0] = fc.fogStart;
    g.fogParams[1] = fc.fogEnd;
    g.fogParams[2] = fc.fogDensity;
    g.fogParams[3] = static_cast<float>(static_cast<int>(fc.fogMode));
    g.ambient[0] = fc.ambient.x;
    g.ambient[1] = fc.ambient.y;
    g.ambient[2] = fc.ambient.z;
    for (int i = 0; i < 3; ++i) {
        const Vec3 d = fc.lights[static_cast<std::size_t>(i)].direction.normalized();
        const Vec3& c = fc.lights[static_cast<std::size_t>(i)].color;
        g.lightDir[i][0] = d.x;
        g.lightDir[i][1] = d.y;
        g.lightDir[i][2] = d.z;
        g.lightColor[i][0] = c.x;
        g.lightColor[i][1] = c.y;
        g.lightColor[i][2] = c.z;
    }
    storeMatrix(g.shadowViewProj, fc.shadowViewProj);
    g.shadowParams[0] = fc.shadowStrength;
    g.shadowParams[1] = fc.shadowTexel;
    g.shadowParams[2] = fc.shadowSize;
    g.shadowParams[3] = fc.shadowSoftness;
    g.surface[0] = fc.surface.x;
    g.surface[1] = fc.surface.y;
    g.surface[2] = fc.surface.z;
    g.surface[3] = fc.surface.w;
    g.ground[0] = fc.ground.x;
    g.ground[1] = fc.ground.y;
    g.ground[2] = fc.ground.z;
    g.ground[3] = fc.ground.w;
    g.haze[0] = fc.haze.x;
    g.haze[1] = fc.haze.y;
    g.haze[2] = fc.haze.z;
    g.haze[3] = fc.haze.w;
    const std::uint32_t lights = std::min<std::uint32_t>(fc.sceneLightCount, kMaxSceneLights);
    g.lightInfo[0] = static_cast<float>(lights);
    for (std::uint32_t i = 0; i < lights; ++i) {
        const SceneLight& l = fc.sceneLights[i];
        g.lightPos[i][0] = l.position.x;
        g.lightPos[i][1] = l.position.y;
        g.lightPos[i][2] = l.position.z;
        g.lightPos[i][3] = l.range;
        g.lightSpot[i][0] = l.direction.x;
        g.lightSpot[i][1] = l.direction.y;
        g.lightSpot[i][2] = l.direction.z;
        g.lightSpot[i][3] = l.cosOuter;
        g.lightTint[i][0] = l.color.x;
        g.lightTint[i][1] = l.color.y;
        g.lightTint[i][2] = l.color.z;
        g.lightTint[i][3] = l.cosInner;
    }
    return g;
}

inline GpuDrawConstants toGpu(const DrawConstants& dc) {
    GpuDrawConstants g{};
    storeMatrix(g.world, dc.world);
    g.color[0] = dc.color.x;
    g.color[1] = dc.color.y;
    g.color[2] = dc.color.z;
    g.color[3] = dc.color.w;
    g.alphaRef = dc.alphaRef;
    g.flags = dc.flags;
    g.specular = dc.specular;
    g.specularPower = dc.specularPower;
    g.emissive[0] = dc.emissive.x;
    g.emissive[1] = dc.emissive.y;
    g.emissive[2] = dc.emissive.z;
    g.emissive[3] = dc.emissive.w;
    return g;
}

} // namespace mm2::render::detail
