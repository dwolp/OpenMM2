#pragma once

// Minimal OpenGL 3.3 core function loader. Function pointers are fetched with
// SDL_GL_GetProcAddress, so no OpenGL import library is linked and no extra
// loader dependency is needed. Only the functions the renderer uses are
// listed; add new ones to MM2_GL_FUNCTIONS.

#include <SDL3/SDL_opengl.h>

#include <string>

#ifndef APIENTRY
#define APIENTRY
#endif

namespace mm2::render::gl {

// X(return type, name without the "gl" prefix, parameter list)
#define MM2_GL_FUNCTIONS(X)                                                                                         \
    X(const GLubyte*, GetString, (GLenum name))                                                                   \
    X(const GLubyte*, GetStringi, (GLenum name, GLuint index))                                                    \
    X(void, GetIntegerv, (GLenum pname, GLint * data))                                                            \
    X(void, GetFloatv, (GLenum pname, GLfloat * data))                                                            \
    X(GLenum, GetError, (void))                                                                                   \
    X(void, Enable, (GLenum cap))                                                                                 \
    X(void, Disable, (GLenum cap))                                                                                \
    X(void, BlendFuncSeparate, (GLenum srcRGB, GLenum dstRGB, GLenum srcA, GLenum dstA))                         \
    X(void, BlendEquation, (GLenum mode))                                                                         \
    X(void, DepthFunc, (GLenum func))                                                                             \
    X(void, DepthMask, (GLboolean flag))                                                                          \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                     \
    X(void, CullFace, (GLenum mode))                                                                              \
    X(void, FrontFace, (GLenum mode))                                                                             \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                                       \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                                   \
    X(void, DepthRange, (GLdouble n, GLdouble f))                                                                 \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                                    \
    X(void, Clear, (GLbitfield mask))                                                                             \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                             \
    X(void, ClearDepth, (GLdouble d))                                                                             \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                                \
    X(void, DrawElementsBaseVertex, (GLenum mode, GLsizei count, GLenum type, const void* indices, GLint base))   \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                             \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data))         \
    X(void, ReadBuffer, (GLenum mode))                                                                            \
    X(void, DrawBuffer, (GLenum mode))                                                                            \
    X(void, Finish, (void))                                                                                       \
    X(void, ActiveTexture, (GLenum texture))                                                                      \
    X(void, GenTextures, (GLsizei n, GLuint * textures))                                                          \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                                  \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                         \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border,    \
                         GLenum format, GLenum type, const void* pixels))                                         \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,    \
                            GLenum type, const void* pixels))                                                     \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                            \
    X(void, GenSamplers, (GLsizei n, GLuint * samplers))                                                          \
    X(void, DeleteSamplers, (GLsizei n, const GLuint* samplers))                                                  \
    X(void, BindSampler, (GLuint unit, GLuint sampler))                                                           \
    X(void, SamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                                       \
    X(void, SamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))                                     \
    X(void, GenBuffers, (GLsizei n, GLuint * buffers))                                                            \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))                                                    \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                           \
    X(void, BindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))      \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                         \
    X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))                   \
    X(void, GenVertexArrays, (GLsizei n, GLuint * arrays))                                                        \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                                \
    X(void, BindVertexArray, (GLuint array))                                                                      \
    X(void, EnableVertexAttribArray, (GLuint index))                                                              \
    X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,     \
                                  const void* pointer))                                                           \
    X(GLuint, CreateShader, (GLenum type))                                                                        \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))        \
    X(void, CompileShader, (GLuint shader))                                                                       \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint * params))                                           \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei maxLength, GLsizei * length, GLchar * infoLog))             \
    X(void, DeleteShader, (GLuint shader))                                                                        \
    X(GLuint, CreateProgram, (void))                                                                              \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                        \
    X(void, LinkProgram, (GLuint program))                                                                        \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint * params))                                         \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei maxLength, GLsizei * length, GLchar * infoLog))           \
    X(void, UseProgram, (GLuint program))                                                                         \
    X(void, DeleteProgram, (GLuint program))                                                                      \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                                            \
    X(void, Uniform1i, (GLint location, GLint v0))                                                                \
    X(GLuint, GetUniformBlockIndex, (GLuint program, const GLchar* name))                                         \
    X(void, UniformBlockBinding, (GLuint program, GLuint blockIndex, GLuint binding))                             \
    X(void, GenFramebuffers, (GLsizei n, GLuint * framebuffers))                                                  \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                                          \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer))                                                 \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer))   \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                            \
    X(void, GenRenderbuffers, (GLsizei n, GLuint * renderbuffers))                                                \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* renderbuffers))                                        \
    X(void, BindRenderbuffer, (GLenum target, GLuint renderbuffer))                                               \
    X(void, RenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h))    \
    X(void, BlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,         \
                              GLint dy1, GLbitfield mask, GLenum filter))

// Optional (KHR_debug / GL 4.3).
using DebugProc = void(APIENTRY*)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                   const GLchar* message, const void* userParam);

struct Api {
#define MM2_GL_DECLARE(ret, name, params) ret(APIENTRY* name) params = nullptr;
    MM2_GL_FUNCTIONS(MM2_GL_DECLARE)
#undef MM2_GL_DECLARE
    void(APIENTRY* DebugMessageCallback)(DebugProc callback, const void* userParam) = nullptr;
    void(APIENTRY* ObjectLabel)(GLenum identifier, GLuint name, GLsizei length, const GLchar* label) = nullptr;

    // Loads all functions from the current context. Returns false and lists
    // the missing ones in `missing` if any required function is absent.
    bool load(std::string* missing);
};

} // namespace mm2::render::gl
