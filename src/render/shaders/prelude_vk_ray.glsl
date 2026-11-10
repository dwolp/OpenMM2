#version 460
// OpenMM2 ray traced sun shadows: mesh.frag with ray queries (Vulkan 1.2,
// VK_KHR_ray_query), put before prelude_vk.glsl (cmake/Shaders.cmake).
#extension GL_EXT_ray_query : require
#define MM2_RAY_QUERY 1
