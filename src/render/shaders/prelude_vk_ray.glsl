#version 460
// OpenMM2 ray traced sun shadows: mesh.frag with ray queries (Vulkan 1.2,
// VK_KHR_ray_query), put before prelude_vk.glsl (cmake/Shaders.cmake). The
// alpha tested casters' vertices are read by address and their textures
// from an array indexed per hit.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// The 2 x 2 pixel quads share their rays (mesh.frag's quad sharing).
#extension GL_KHR_shader_subgroup_quad : require
#define MM2_RAY_QUERY 1
