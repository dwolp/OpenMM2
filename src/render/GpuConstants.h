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
