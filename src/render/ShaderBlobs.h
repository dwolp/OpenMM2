#pragma once

// Shaders embedded at build time by cmake/Shaders.cmake (see shaders/).

#include <cstddef>
#include <cstdint>
#include <span>

namespace mm2::render::shaders {

#define MM2_DECLARE_SHADER(name)                                                                                      \
    extern const std::uint8_t name##_spv[];                                                                          \
    extern const std::size_t name##_spv_size;                                                                        \
    extern const char name##_glsl[];

MM2_DECLARE_SHADER(mesh_vert)
MM2_DECLARE_SHADER(mesh_frag)
MM2_DECLARE_SHADER(mesh_ray_frag) // Vulkan only: mesh.frag with ray queries (OpenMM2 ray traced shadows)
MM2_DECLARE_SHADER(overlay_vert)
MM2_DECLARE_SHADER(overlay_frag)
MM2_DECLARE_SHADER(composite_vert)
MM2_DECLARE_SHADER(composite_frag)
MM2_DECLARE_SHADER(bloom_frag)
MM2_DECLARE_SHADER(depthcopy_frag)
MM2_DECLARE_SHADER(depthcopy_ms_frag)
MM2_DECLARE_SHADER(ao_frag)
MM2_DECLARE_SHADER(aoblur_frag)
MM2_DECLARE_SHADER(ssr_frag)
MM2_DECLARE_SHADER(shafts_frag)
MM2_DECLARE_SHADER(motionblur_frag)
MM2_DECLARE_SHADER(taa_frag)

#undef MM2_DECLARE_SHADER

} // namespace mm2::render::shaders

#define MM2_SHADER_SPV(name)                                                                                          \
    std::span<const std::uint8_t>(::mm2::render::shaders::name##_spv, ::mm2::render::shaders::name##_spv_size)
#define MM2_SHADER_GLSL(name) (::mm2::render::shaders::name##_glsl)
