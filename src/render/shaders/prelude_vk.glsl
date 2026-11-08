#version 450
#define MM2_VULKAN 1
#define ATTR(n) layout(location = n) in
#define IN(n) layout(location = n) in
#define OUT(n) layout(location = n) out
#define FRAME_BLOCK layout(set = 0, binding = 0, std140) uniform FrameBlock
#define DRAW_BLOCK layout(push_constant, std430) uniform DrawBlock
#define TEXTURE0 layout(set = 1, binding = 0) uniform sampler2D
#define TEXTURE1 layout(set = 2, binding = 0) uniform sampler2D
#define TEXTURE2 layout(set = 3, binding = 0) uniform sampler2D
#define VERTEX_ID gl_VertexIndex
// Vulkan: clip-space y is flipped by a negative viewport height, depth is 0..1.
vec4 clipFixup(vec4 p) { return p; }
