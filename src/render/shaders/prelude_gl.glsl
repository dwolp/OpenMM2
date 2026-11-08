#version 330 core
#define MM2_OPENGL 1
#define ATTR(n) layout(location = n) in
#define IN(n) in
#define OUT(n) out
#define FRAME_BLOCK layout(std140) uniform FrameBlock
#define DRAW_BLOCK layout(std140) uniform DrawBlock
#define TEXTURE0 uniform sampler2D
#define TEXTURE1 uniform sampler2D
#define TEXTURE2 uniform sampler2D
#define VERTEX_ID gl_VertexID
// The engine uses Direct3D-style 0..1 depth; OpenGL 3.3 clips to -1..1.
vec4 clipFixup(vec4 p) { return vec4(p.x, p.y, p.z * 2.0 - p.w, p.w); }
