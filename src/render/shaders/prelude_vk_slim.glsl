#version 460
// OpenMM2 ray traced sun shadows, slim: mesh.frag with ray queries for the
// sun's rays only (MM2_RAY_SLIM: no reflections), for the grass shells,
// whose 16 layers the full shader's size slowed; put before prelude_vk.glsl. The
// alpha tested casters' vertices are read by address and their textures
// from an array indexed per hit.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#define MM2_RAY_QUERY 1
#define MM2_RAY_SLIM 1
