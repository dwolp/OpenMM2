#pragma once

// std140 mirrors of the shader constant blocks (see shaders/common.glsl).
// Shared by the Vulkan and OpenGL backends.

#include "render/Types.h"

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
};
static_assert(sizeof(GpuFrameConstants) == 352);

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
};
static_assert(sizeof(GpuPostConstants) <= sizeof(GpuDrawConstants));

inline GpuPostConstants toGpu(const PostProcess& p) {
    return {{p.contrast, p.saturation, p.warmth, p.vignette},
            {p.sharpen, p.fxaa ? 1.0f : 0.0f, p.active() ? 1.0f : 0.0f, p.bloom}};
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
