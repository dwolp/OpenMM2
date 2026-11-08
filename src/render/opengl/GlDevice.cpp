// OpenGL 3.3 core backend.
//
// Structure mirrors the Vulkan backend: the 3D scene is rendered into an
// offscreen framebuffer (multisampled when MSAA is on, sized by the render
// scale), resolved, and drawn onto the default framebuffer by a full-screen
// "composite" triangle; overlays are then drawn at native resolution.
// Transient geometry and constants go into per-frame ring buffers that are
// orphaned at the start of each frame.

#include "core/Log.h"
#include "platform/Window.h"
#include "render/Device.h"
#include "render/GpuConstants.h"
#include "render/HandleTable.h"
#include "render/ImageUtil.h"
#include "render/Projection.h"
#include "render/ShaderBlobs.h"
#include "render/opengl/GlApi.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string_view>
#include <unordered_map>

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

namespace mm2::render {
namespace {

constexpr std::size_t kRingChunkSize = 4u << 20;

struct GlTexture {
    GLuint id = 0;
    std::uint32_t width = 0, height = 0, mips = 1;
};

struct GlBuffer {
    GLuint id = 0;
    std::size_t size = 0;
};

// Growable per-frame upload area made of fixed GL buffers. A slice handed
// out stays valid for the whole frame: when a chunk fills up we move on to
// another buffer object instead of orphaning the one still referenced.
struct Ring {
    struct Chunk {
        std::uint32_t handle = 0; // id in GlDevice::m_buffers
        GLuint gl = 0;
        std::size_t size = 0;
        std::size_t offset = 0;
    };
    std::vector<Chunk> chunks;
    std::size_t current = 0;
};

GLenum toGl(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return GL_NEVER;
    case CompareOp::Less: return GL_LESS;
    case CompareOp::Equal: return GL_EQUAL;
    case CompareOp::LessEqual: return GL_LEQUAL;
    case CompareOp::Greater: return GL_GREATER;
    case CompareOp::NotEqual: return GL_NOTEQUAL;
    case CompareOp::GreaterEqual: return GL_GEQUAL;
    case CompareOp::Always: return GL_ALWAYS;
    }
    return GL_LEQUAL;
}

GLenum toGl(Topology t) {
    switch (t) {
    case Topology::TriangleList: return GL_TRIANGLES;
    case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
    case Topology::LineList: return GL_LINES;
    }
    return GL_TRIANGLES;
}

GLint toGl(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap: return GL_REPEAT;
    case AddressMode::Clamp: return GL_CLAMP_TO_EDGE;
    case AddressMode::Mirror: return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

void APIENTRY debugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message,
                            const void* user) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
        return;
    auto* errors = static_cast<int*>(const_cast<void*>(user));
    if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) {
        ++*errors;
        log::error("gl: [{}] {}", id, message);
    } else {
        log::warn("gl: [{}] {}", id, message);
    }
}

class GlDevice final : public Device {
public:
    explicit GlDevice(platform::Window& window) : m_window(window) {}
    ~GlDevice() override;

    bool init(const DeviceCreateInfo& info, std::string* error);

    const DeviceInfo& info() const override { return m_info; }

    TextureHandle createTexture(const TextureDesc& desc, std::span<const TextureData> mips) override;
    void updateTexture(TextureHandle texture, std::uint32_t mip, const Rect& region, const void* data,
                       std::uint32_t rowPitch) override;
    void destroyTexture(TextureHandle texture) override;
    BufferHandle createBuffer(BufferKind kind, std::size_t size, const void* data) override;
    void updateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) override;
    void destroyBuffer(BufferHandle buffer) override;
    BufferSlice uploadTransient(BufferKind kind, std::span<const std::byte> data) override;

    void applySettings(const DisplaySettings& settings) override;
    void notifyResized() override { m_resizePending = true; }

    bool beginFrame() override;
    bool beginShadowPass(std::uint32_t size) override;
    void endShadowPass() override;
    Extent2D outputExtent() const override { return m_output; }
    Extent2D sceneExtent() const override { return m_sceneExtent; }
    void beginScene(const ClearValues& clear) override;
    void endScene() override;
    void beginOverlay(const Vec4& clearColor) override;
    void endOverlay() override;
    void endFrame() override;

    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect* rect) override;
    void clear(const ClearValues& values) override;
    void setFrameConstants(const FrameConstants& constants) override;
    void draw(const DrawCall& call) override;

    void requestCapture() override { m_captureRequested = true; }
    bool readCapture(Image& out) override;
    void waitIdle() override { gl.Finish(); }
    const FrameStats& stats() const override { return m_stats; }

private:
    enum class Pass { None, Scene, Overlay, Shadow };

    bool hasExtension(std::string_view name) const;
    GLuint compileProgram(const char* vs, const char* fs, std::string* error);
    void createSceneTargets();
    void destroySceneTargets();
    // OpenMM2 post-processing chain (bloom): textures at fractions of the
    // scene target, drawn with full-screen passes before the composite.
    void createPostTargets();
    void destroyPostTargets();
    struct PostTarget;
    void beginPostPasses();
    // One full-screen pass into `dst` with `program`, textures on units 0
    // and 1 (tex1 0: tex0 again) and `size` bytes of DrawBlock constants.
    void drawPostPass(const PostTarget& dst, GLuint program, GLuint tex0, GLuint sampler0, GLuint tex1,
                      GLuint sampler1, const void* constants, std::size_t size);
    void runAo();
    void runBloom();
    // The scene image the post passes and the composite read: the shaded
    // one with ambient occlusion this frame.
    GLuint postSourceTex() const;
    void applySwapInterval();
    GLuint sampler(const SamplerDesc& desc);
    void rebuildSamplers();
    std::pair<GLuint, std::size_t> ringAlloc(Ring& ring, std::size_t size, std::size_t align, std::uint32_t* handle);
    void resetRing(Ring& ring);
    void applyState(const PipelineState& s, const DrawCall& call);
    void invalidateState() { m_stateValid = false; }
    std::uint32_t targetHeight() const {
        if (m_pass == Pass::Shadow)
            return m_shadowSize;
        return m_pass == Pass::Scene ? m_sceneExtent.height : m_output.height;
    }
    void setDefaultViewport();

    platform::Window& m_window;
    SDL_GLContext m_context = nullptr;
    gl::Api gl;
    DeviceInfo m_info;
    DisplaySettings m_settings;
    FrameStats m_stats;
    int m_debugErrors = 0;

    detail::HandleTable<GlTexture> m_textures;
    detail::HandleTable<GlBuffer> m_buffers;
    std::unordered_map<std::uint32_t, GLuint> m_samplers;
    GLuint m_white = 0;
    float m_maxAniso = 1.0f;
    GLint m_uboAlign = 256;
    GLint m_maxSamples = 1;

    GLuint m_progMesh = 0, m_progOverlay = 0, m_progComposite = 0;
    GLuint m_vaoMesh = 0, m_vaoOverlay = 0, m_vaoEmpty = 0;
    struct VaoBinding {
        GLuint vb = 0;
        std::size_t offset = ~std::size_t{0};
        GLuint ib = ~0u;
    } m_vaoState[2];
    GLuint m_currentVao = 0;
    GLuint m_currentProgram = 0;

    Ring m_vertexRing, m_indexRing, m_uniformRing;

    // Scene targets.
    Extent2D m_output, m_sceneExtent;
    std::uint32_t m_samples = 1;
    GLuint m_sceneFbo = 0, m_resolveFbo = 0, m_sceneTex = 0, m_colorRb = 0, m_depthRb = 0;
    struct PostTarget {
        GLuint tex = 0, fbo = 0;
        std::uint32_t width = 0, height = 0;
    };
    std::array<PostTarget, detail::kBloomLevels> m_bloomDown;
    std::array<PostTarget, detail::kBloomLevels - 1> m_bloomUp;
    GLuint m_progBloom = 0, m_progDepthCopy = 0, m_progAo = 0, m_progAoBlur = 0;
    bool m_bloomThisFrame = false; // m_bloomUp[0] holds this frame's glow
    // Ambient occlusion: distances, occlusion and distance, the shaded scene,
    // and the scene depth blitted into a texture.
    PostTarget m_aoDepth, m_aoRaw, m_aoScene;
    PostTarget m_ssrScene;   // wet roads: the scene with its reflections
    PostTarget m_shaftScene; // the scene with sun shafts
    PostTarget m_blurScene;  // the scene with motion blur
    // Motion blur: the previous frame's scene camera, projection and time.
    std::optional<Mat44> m_prevProjection;
    std::optional<Mat34> m_prevCamera;
    float m_prevTime = 0.0f;
    const PostTarget* m_postCurrent = nullptr; // the last post image written this frame (null: the scene)
    GLuint m_progSsr = 0, m_progShafts = 0, m_progBlur = 0;
    bool m_ssrThisFrame = false; // m_ssrScene holds this frame's wet scene
    GLuint m_depthTex = 0, m_depthFbo = 0;
    bool m_aoThisFrame = false; // m_aoScene holds this frame's shaded scene
    // Sun shadows (beginShadowPass): the shadow map, unit 2 of mesh draws.
    GLuint m_shadowTex = 0, m_shadowFbo = 0;
    std::uint32_t m_shadowSize = 0;
    bool m_shadowThisFrame = false; // the map holds this frame's shadows
    GLuint m_boundShadowTex = ~0u;

    Pass m_pass = Pass::None;
    bool m_inFrame = false;
    bool m_sceneRendered = false;
    bool m_resizePending = true;
    Viewport m_viewport;
    bool m_scissorOn = false;
    Rect m_scissor;

    PipelineState m_state;
    bool m_stateValid = false;
    GLuint m_boundTex[2] = {~0u, ~0u};
    GLuint m_boundSampler[2] = {~0u, ~0u};

    bool m_captureRequested = false;
    bool m_captureReady = false;
    Image m_capture;
};

GlDevice::~GlDevice() {
    if (!m_context)
        return;
    SDL_GL_MakeCurrent(m_window.sdl(), m_context);
    destroySceneTargets();
    m_textures.forEach([&](std::uint32_t, GlTexture& t) { gl.DeleteTextures(1, &t.id); });
    m_buffers.forEach([&](std::uint32_t, GlBuffer& b) { gl.DeleteBuffers(1, &b.id); });
    for (auto& [k, s] : m_samplers)
        gl.DeleteSamplers(1, &s);
    for (GLuint p : {m_progMesh, m_progOverlay, m_progComposite, m_progBloom, m_progDepthCopy, m_progAo, m_progAoBlur,
                     m_progSsr, m_progShafts, m_progBlur})
        if (p)
            gl.DeleteProgram(p);
    for (GLuint v : {m_vaoMesh, m_vaoOverlay, m_vaoEmpty})
        if (v)
            gl.DeleteVertexArrays(1, &v);
    SDL_GL_DestroyContext(m_context);
}

bool GlDevice::hasExtension(std::string_view name) const {
    GLint n = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const auto* e = reinterpret_cast<const char*>(gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e && name == e)
            return true;
    }
    return false;
}

bool GlDevice::init(const DeviceCreateInfo& ci, std::string* error) {
    m_settings = ci.settings;
    m_settings.sanitize();
    m_context = SDL_GL_CreateContext(m_window.sdl());
    if (!m_context) {
        *error = std::format("cannot create an OpenGL 3.3 core context: {}", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_window.sdl(), m_context);
    std::string missing;
    if (!gl.load(&missing)) {
        *error = "OpenGL functions missing: " + missing;
        return false;
    }
    GLint major = 0, minor = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &major);
    gl.GetIntegerv(GL_MINOR_VERSION, &minor);
    if (major * 10 + minor < 33) {
        *error = std::format("OpenGL 3.3 required, driver provides {}.{}", major, minor);
        return false;
    }

    auto str = [&](GLenum e) {
        const auto* s = reinterpret_cast<const char*>(gl.GetString(e));
        return std::string(s ? s : "");
    };
    m_info.backend = Backend::OpenGL;
    m_info.apiVersion = "OpenGL " + str(GL_VERSION);
    m_info.deviceName = str(GL_RENDERER);
    m_info.driverInfo = str(GL_VENDOR);
    GLint maxTex = 0;
    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    m_info.maxTextureSize = static_cast<std::uint32_t>(maxTex);
    gl.GetIntegerv(GL_MAX_SAMPLES, &m_maxSamples);
    for (std::uint32_t s = 1; s <= 8 && static_cast<GLint>(s) <= std::max(m_maxSamples, 1); s *= 2)
        m_info.msaaSamples.push_back(s);
    if (major * 10 + minor >= 46 || hasExtension("GL_ARB_texture_filter_anisotropic") ||
        hasExtension("GL_EXT_texture_filter_anisotropic")) {
        gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m_maxAniso);
    }
    m_info.maxAnisotropy = m_maxAniso;
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &m_uboAlign);
    m_uboAlign = std::max<GLint>(m_uboAlign, 16);

    if (m_settings.validation && gl.DebugMessageCallback) {
        gl.Enable(GL_DEBUG_OUTPUT);
        gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        gl.DebugMessageCallback(debugCallback, &m_debugErrors);
        log::info("gl: debug output enabled");
    }

    std::string perr;
    m_progMesh = compileProgram(MM2_SHADER_GLSL(mesh_vert), MM2_SHADER_GLSL(mesh_frag), &perr);
    if (m_progMesh)
        m_progOverlay = compileProgram(MM2_SHADER_GLSL(overlay_vert), MM2_SHADER_GLSL(overlay_frag), &perr);
    if (m_progOverlay)
        m_progComposite = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(composite_frag), &perr);
    if (m_progComposite)
        m_progBloom = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(bloom_frag), &perr);
    if (m_progBloom)
        m_progDepthCopy = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(depthcopy_frag), &perr);
    if (m_progDepthCopy)
        m_progAo = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(ao_frag), &perr);
    if (m_progAo)
        m_progAoBlur = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(aoblur_frag), &perr);
    if (m_progAoBlur)
        m_progSsr = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(ssr_frag), &perr);
    if (m_progSsr)
        m_progShafts = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(shafts_frag), &perr);
    if (m_progShafts)
        m_progBlur = compileProgram(MM2_SHADER_GLSL(composite_vert), MM2_SHADER_GLSL(motionblur_frag), &perr);
    if (!m_progBlur) {
        *error = "shader compilation failed: " + perr;
        return false;
    }

    // One VAO per vertex format, plus an empty one for attribute-less draws.
    gl.GenVertexArrays(1, &m_vaoMesh);
    gl.GenVertexArrays(1, &m_vaoOverlay);
    gl.GenVertexArrays(1, &m_vaoEmpty);
    gl.BindVertexArray(m_vaoMesh);
    for (GLuint i = 0; i < 5; ++i)
        gl.EnableVertexAttribArray(i);
    gl.BindVertexArray(m_vaoOverlay);
    for (GLuint i = 0; i < 3; ++i)
        gl.EnableVertexAttribArray(i);
    gl.BindVertexArray(0);

    const std::uint32_t white = 0xFFFFFFFFu;
    TextureDesc wd;
    wd.debugName = "white";
    const TextureData wdata{&white, 0};
    m_white = m_textures.get(createTexture(wd, std::span(&wdata, 1)).id)->id;

    applySwapInterval();
    m_resizePending = true;
    return true;
}

GLuint GlDevice::compileProgram(const char* vsSrc, const char* fsSrc, std::string* error) {
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = gl.CreateShader(type);
        gl.ShaderSource(s, 1, &src, nullptr);
        gl.CompileShader(s);
        GLint ok = 0;
        gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char buf[4096];
            GLsizei len = 0;
            gl.GetShaderInfoLog(s, sizeof(buf), &len, buf);
            *error = std::string(buf, static_cast<std::size_t>(len));
            gl.DeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
    if (!vs)
        return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (!fs) {
        gl.DeleteShader(vs);
        return 0;
    }
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        GLsizei len = 0;
        gl.GetProgramInfoLog(p, sizeof(buf), &len, buf);
        *error = std::string(buf, static_cast<std::size_t>(len));
        gl.DeleteProgram(p);
        return 0;
    }
    // GLSL 3.30 has no binding qualifiers: assign them here.
    if (GLuint b = gl.GetUniformBlockIndex(p, "FrameBlock"); b != GL_INVALID_INDEX)
        gl.UniformBlockBinding(p, b, 0);
    if (GLuint b = gl.GetUniformBlockIndex(p, "DrawBlock"); b != GL_INVALID_INDEX)
        gl.UniformBlockBinding(p, b, 1);
    gl.UseProgram(p);
    for (auto [name, unit] : {std::pair{"uTexture0", 0}, {"uTexture1", 1}, {"uScene", 0}, {"uShadow", 2}, {"uRaw", 2}})
        if (GLint loc = gl.GetUniformLocation(p, name); loc >= 0)
            gl.Uniform1i(loc, unit);
    gl.UseProgram(0);
    m_currentProgram = 0;
    return p;
}

void GlDevice::applySwapInterval() {
    int interval = 1;
    switch (m_settings.vsync) {
    case VsyncMode::Off: interval = 0; break;
    case VsyncMode::On: interval = 1; break;
    case VsyncMode::Mailbox: interval = 1; break; // no mailbox in OpenGL; closest tear-free mode
    case VsyncMode::Adaptive: interval = -1; break;
    }
    if (!SDL_GL_SetSwapInterval(interval) && interval == -1)
        SDL_GL_SetSwapInterval(1);
}

void GlDevice::applySettings(const DisplaySettings& settings) {
    DisplaySettings s = settings;
    s.sanitize();
    const bool targets = s.msaa != m_settings.msaa || s.renderScale != m_settings.renderScale;
    const bool aniso = s.anisotropy != m_settings.anisotropy;
    const bool vsync = s.vsync != m_settings.vsync;
    m_settings = s;
    if (vsync)
        applySwapInterval();
    if (aniso)
        rebuildSamplers();
    if (targets)
        m_resizePending = true;
}

// --- Textures ----------------------------------------------------------------

TextureHandle GlDevice::createTexture(const TextureDesc& desc, std::span<const TextureData> mips) {
    GlTexture t;
    t.width = std::max(desc.width, 1u);
    t.height = std::max(desc.height, 1u);
    t.mips = std::clamp(desc.mipLevels, 1u, mipCount(t.width, t.height));
    gl.GenTextures(1, &t.id);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, t.id);
    m_boundTex[0] = ~0u;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(t.mips - 1));
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (std::uint32_t level = 0; level < t.mips; ++level) {
        const auto w = static_cast<GLsizei>(std::max(1u, t.width >> level));
        const auto h = static_cast<GLsizei>(std::max(1u, t.height >> level));
        const TextureData* d = level < mips.size() ? &mips[level] : nullptr;
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, d && d->rowPitch ? static_cast<GLint>(d->rowPitch / 4) : 0);
        gl.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      d ? d->data : nullptr);
    }
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (gl.ObjectLabel && !desc.debugName.empty())
        gl.ObjectLabel(GL_TEXTURE, t.id, -1, desc.debugName.c_str());
    return TextureHandle{m_textures.insert(t)};
}

void GlDevice::updateTexture(TextureHandle handle, std::uint32_t mip, const Rect& r, const void* data,
                             std::uint32_t rowPitch) {
    const GlTexture* t = m_textures.get(handle.id);
    if (!t || mip >= t->mips || !data)
        return;
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, t->id);
    m_boundTex[0] = ~0u;
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowPitch ? static_cast<GLint>(rowPitch / 4) : 0);
    gl.TexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(mip), r.x, r.y, static_cast<GLsizei>(r.width),
                     static_cast<GLsizei>(r.height), GL_RGBA, GL_UNSIGNED_BYTE, data);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void GlDevice::destroyTexture(TextureHandle handle) {
    if (auto t = m_textures.remove(handle.id)) {
        gl.DeleteTextures(1, &t->id);
        m_boundTex[0] = m_boundTex[1] = ~0u;
    }
}

GLuint GlDevice::sampler(const SamplerDesc& desc) {
    const std::uint32_t key = desc.key();
    if (auto it = m_samplers.find(key); it != m_samplers.end())
        return it->second;
    GLuint s = 0;
    gl.GenSamplers(1, &s);
    GLint minF = GL_LINEAR_MIPMAP_LINEAR, magF = GL_LINEAR;
    switch (desc.filter) {
    case Filter::Point:
        minF = GL_NEAREST_MIPMAP_NEAREST;
        magF = GL_NEAREST;
        break;
    case Filter::Bilinear: minF = GL_LINEAR_MIPMAP_NEAREST; break;
    case Filter::Trilinear: break;
    }
    gl.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minF);
    gl.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, magF);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_S, toGl(desc.addressU));
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_T, toGl(desc.addressV));
    if (desc.filter == Filter::Trilinear && m_maxAniso > 1.0f && m_settings.anisotropy > 1)
        gl.SamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY,
                             std::min(m_maxAniso, static_cast<float>(m_settings.anisotropy)));
    m_samplers.emplace(key, s);
    return s;
}

void GlDevice::rebuildSamplers() {
    for (auto& [k, s] : m_samplers)
        gl.DeleteSamplers(1, &s);
    m_samplers.clear();
    m_boundSampler[0] = m_boundSampler[1] = ~0u;
}

// --- Buffers -------------------------------------------------------------------

BufferHandle GlDevice::createBuffer(BufferKind, std::size_t size, const void* data) {
    GlBuffer b;
    b.size = size;
    gl.GenBuffers(1, &b.id);
    // GL_COPY_WRITE_BUFFER is not VAO state, so uploads never disturb draws.
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, b.id);
    gl.BufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(size), data, GL_STATIC_DRAW);
    return BufferHandle{m_buffers.insert(b)};
}

void GlDevice::updateBuffer(BufferHandle handle, std::size_t offset, std::span<const std::byte> data) {
    const GlBuffer* b = m_buffers.get(handle.id);
    if (!b || offset + data.size() > b->size)
        return;
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, b->id);
    gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()),
                     data.data());
}

void GlDevice::destroyBuffer(BufferHandle handle) {
    if (auto b = m_buffers.remove(handle.id)) {
        gl.DeleteBuffers(1, &b->id);
        for (auto& v : m_vaoState)
            v = {};
    }
}

std::pair<GLuint, std::size_t> GlDevice::ringAlloc(Ring& ring, std::size_t size, std::size_t align,
                                                   std::uint32_t* handle) {
    while (true) {
        if (ring.current >= ring.chunks.size()) {
            Ring::Chunk c;
            c.size = std::max(kRingChunkSize, (size + align) * 2);
            gl.GenBuffers(1, &c.gl);
            gl.BindBuffer(GL_COPY_WRITE_BUFFER, c.gl);
            gl.BufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(c.size), nullptr, GL_STREAM_DRAW);
            c.handle = m_buffers.insert(GlBuffer{c.gl, c.size});
            ring.chunks.push_back(c);
        }
        Ring::Chunk& c = ring.chunks[ring.current];
        const std::size_t offset = (c.offset + align - 1) / align * align;
        if (offset + size <= c.size) {
            c.offset = offset + size;
            if (handle)
                *handle = c.handle;
            return {c.gl, offset};
        }
        ++ring.current;
    }
}

void GlDevice::resetRing(Ring& ring) {
    for (auto& c : ring.chunks) {
        if (c.offset == 0)
            continue;
        // Orphan: the driver hands us fresh storage, no waiting on the GPU.
        gl.BindBuffer(GL_COPY_WRITE_BUFFER, c.gl);
        gl.BufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(c.size), nullptr, GL_STREAM_DRAW);
        c.offset = 0;
    }
    ring.current = 0;
}

BufferSlice GlDevice::uploadTransient(BufferKind kind, std::span<const std::byte> data) {
    Ring& ring = kind == BufferKind::Vertex ? m_vertexRing : m_indexRing;
    std::uint32_t handle = 0;
    const auto [buf, offset] = ringAlloc(ring, std::max<std::size_t>(data.size(), 4), 16, &handle);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buf);
    gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()),
                     data.data());
    m_stats.transientBytes += data.size();
    return {BufferHandle{handle}, static_cast<std::uint32_t>(offset)};
}

// --- Targets -------------------------------------------------------------------

void GlDevice::destroySceneTargets() {
    destroyPostTargets();
    if (m_shadowFbo)
        gl.DeleteFramebuffers(1, &m_shadowFbo);
    if (m_shadowTex)
        gl.DeleteTextures(1, &m_shadowTex);
    m_shadowFbo = m_shadowTex = 0;
    m_shadowSize = 0;
    m_shadowThisFrame = false;
    m_boundShadowTex = ~0u;
    if (m_sceneFbo)
        gl.DeleteFramebuffers(1, &m_sceneFbo);
    if (m_resolveFbo)
        gl.DeleteFramebuffers(1, &m_resolveFbo);
    if (m_sceneTex)
        gl.DeleteTextures(1, &m_sceneTex);
    if (m_colorRb)
        gl.DeleteRenderbuffers(1, &m_colorRb);
    if (m_depthRb)
        gl.DeleteRenderbuffers(1, &m_depthRb);
    m_sceneFbo = m_resolveFbo = m_sceneTex = m_colorRb = m_depthRb = 0;
}

void GlDevice::createSceneTargets() {
    destroySceneTargets();
    m_sceneExtent = scaledExtent(m_output, m_settings.renderScale);
    m_samples = std::min<std::uint32_t>(m_settings.msaa, static_cast<std::uint32_t>(std::max(m_maxSamples, 1)));
    const auto w = static_cast<GLsizei>(m_sceneExtent.width), h = static_cast<GLsizei>(m_sceneExtent.height);

    gl.GenTextures(1, &m_sceneTex);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, m_sceneTex);
    m_boundTex[0] = ~0u;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    gl.GenRenderbuffers(1, &m_depthRb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, m_depthRb);
    gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples > 1 ? static_cast<GLsizei>(m_samples) : 0,
                                      GL_DEPTH_COMPONENT32F, w, h);

    gl.GenFramebuffers(1, &m_sceneFbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
    if (m_samples > 1) {
        gl.GenRenderbuffers(1, &m_colorRb);
        gl.BindRenderbuffer(GL_RENDERBUFFER, m_colorRb);
        gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, static_cast<GLsizei>(m_samples), GL_RGBA8, w, h);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_colorRb);
    } else {
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneTex, 0);
    }
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRb);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        log::error("gl: scene framebuffer incomplete");

    if (m_samples > 1) {
        gl.GenFramebuffers(1, &m_resolveFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_resolveFbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneTex, 0);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            log::error("gl: resolve framebuffer incomplete");
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    log::debug("gl: scene target {}x{} ({}x MSAA), output {}x{}", w, h, m_samples, m_output.width, m_output.height);
    createPostTargets();
}

void GlDevice::createPostTargets() {
    auto make = [&](PostTarget& t, GLenum internalFormat, GLenum format, GLenum type, std::uint32_t w,
                    std::uint32_t h) {
        t.width = w;
        t.height = h;
        gl.GenTextures(1, &t.tex);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, t.tex);
        m_boundTex[0] = ~0u;
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), static_cast<GLsizei>(w),
                      static_cast<GLsizei>(h), 0, format, type, nullptr);
        gl.GenFramebuffers(1, &t.fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            log::error("gl: post framebuffer incomplete");
    };
    std::uint32_t w = m_sceneExtent.width, h = m_sceneExtent.height;
    for (int i = 0; i < detail::kBloomLevels; ++i) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        make(m_bloomDown[static_cast<std::size_t>(i)], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h);
        if (i + 1 < detail::kBloomLevels)
            make(m_bloomUp[static_cast<std::size_t>(i)], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h);
    }
    // Ambient occlusion: the scene depth blitted to a texture, distances,
    // occlusion and distance, the shaded scene.
    const std::uint32_t sw = m_sceneExtent.width, sh = m_sceneExtent.height;
    make(m_aoDepth, GL_R32F, GL_RED, GL_FLOAT, sw, sh);
    make(m_aoRaw, GL_RG16F, GL_RG, GL_FLOAT, sw, sh);
    make(m_aoScene, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, sw, sh);
    make(m_ssrScene, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, sw, sh);
    make(m_shaftScene, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, sw, sh);
    make(m_blurScene, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, sw, sh);
    gl.GenTextures(1, &m_depthTex);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, m_depthTex);
    m_boundTex[0] = ~0u;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, static_cast<GLsizei>(sw), static_cast<GLsizei>(sh), 0,
                  GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    gl.GenFramebuffers(1, &m_depthFbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, m_depthFbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depthTex, 0);
    gl.DrawBuffer(GL_NONE);
    gl.ReadBuffer(GL_NONE);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        log::error("gl: depth copy framebuffer incomplete");
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    m_bloomThisFrame = false;
    m_aoThisFrame = false;
}

void GlDevice::destroyPostTargets() {
    auto destroy = [&](PostTarget& t) {
        if (t.fbo)
            gl.DeleteFramebuffers(1, &t.fbo);
        if (t.tex)
            gl.DeleteTextures(1, &t.tex);
        t = {};
    };
    for (auto& t : m_bloomDown)
        destroy(t);
    for (auto& t : m_bloomUp)
        destroy(t);
    for (PostTarget* t : {&m_aoDepth, &m_aoRaw, &m_aoScene, &m_ssrScene, &m_shaftScene, &m_blurScene})
        destroy(*t);
    m_ssrThisFrame = false;
    m_postCurrent = nullptr;
    if (m_depthFbo)
        gl.DeleteFramebuffers(1, &m_depthFbo);
    if (m_depthTex)
        gl.DeleteTextures(1, &m_depthTex);
    m_depthFbo = m_depthTex = 0;
    m_bloomThisFrame = false;
    m_aoThisFrame = false;
}

void GlDevice::beginPostPasses() {
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_SCISSOR_TEST);
    m_scissorOn = false;
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    invalidateState();
    gl.BindVertexArray(m_vaoEmpty);
    m_currentVao = m_vaoEmpty;
}

void GlDevice::drawPostPass(const PostTarget& dst, GLuint program, GLuint tex0, GLuint sampler0, GLuint tex1,
                            GLuint sampler1, const void* constants, std::size_t size) {
    gl.UseProgram(program);
    m_currentProgram = program;
    gl.BindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    gl.Viewport(0, 0, static_cast<GLsizei>(dst.width), static_cast<GLsizei>(dst.height));
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, tex0);
    gl.BindSampler(0, sampler0);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, tex1 ? tex1 : tex0);
    gl.BindSampler(1, sampler1 ? sampler1 : sampler0);
    gl.ActiveTexture(GL_TEXTURE0);
    const auto [ubuf, uoff] = ringAlloc(m_uniformRing, size, static_cast<std::size_t>(m_uboAlign), nullptr);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, ubuf);
    gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(uoff), static_cast<GLsizeiptr>(size), constants);
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 1, ubuf, static_cast<GLintptr>(uoff), static_cast<GLsizeiptr>(size));
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    m_boundTex[0] = m_boundTex[1] = ~0u;
    m_boundSampler[0] = m_boundSampler[1] = ~0u;
}

GLuint GlDevice::postSourceTex() const { return m_postCurrent ? m_postCurrent->tex : m_sceneTex; }

void GlDevice::runAo() {
    const PostProcess& post = postProcess();
    m_aoThisFrame = false;
    m_ssrThisFrame = false;
    m_postCurrent = nullptr;
    // The distance image feeds the ambient occlusion, the wet roads and the
    // sun shafts.
    const bool ao = post.ao > 0.0f;
    const bool wet = post.wetness > 0.0f && sceneCamera().has_value();
    const bool sun = post.shafts > 0.0f && sceneCamera().has_value() && sceneSun().has_value();
    const bool moving = post.motionBlur > 0.0f && sceneCamera().has_value() && m_prevCamera && m_prevProjection;
    // The camera for the next frame's motion blur (or none).
    struct KeepPrevious {
        GlDevice& d;
        ~KeepPrevious() {
            d.m_prevProjection = d.sceneProjection();
            d.m_prevCamera = d.sceneCamera();
            d.m_prevTime = d.postProcess().time;
        }
    } keep{*this};
    if (!m_sceneRendered || !(ao || wet || sun || moving) || !sceneProjection() || !m_depthFbo)
        return;
    // The scene depth (multisampled or not) into a plain depth texture.
    const auto w = static_cast<GLint>(m_sceneExtent.width), h = static_cast<GLint>(m_sceneExtent.height);
    gl.Disable(GL_SCISSOR_TEST);
    m_scissorOn = false;
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depthFbo);
    gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    beginPostPasses();
    const GLuint point = sampler({Filter::Point, AddressMode::Clamp, AddressMode::Clamp});
    const GLuint linear = sampler({Filter::Bilinear, AddressMode::Clamp, AddressMode::Clamp});
    const Mat44& proj = *sceneProjection();
    const auto copy = detail::depthCopyConstants(proj);
    drawPostPass(m_aoDepth, m_progDepthCopy, m_depthTex, point, 0, 0, &copy, sizeof(copy));
    if (ao) {
        const auto aoc = detail::aoConstants(proj, post, m_aoDepth.width, m_aoDepth.height);
        drawPostPass(m_aoRaw, m_progAo, m_aoDepth.tex, linear, 0, 0, &aoc, sizeof(aoc));
        const auto blur = detail::aoBlurConstants(post, m_aoRaw.width, m_aoRaw.height);
        drawPostPass(m_aoScene, m_progAoBlur, m_aoRaw.tex, linear, m_sceneTex, linear, &blur, sizeof(blur));
        m_aoThisFrame = true;
        m_postCurrent = &m_aoScene;
    }
    if (wet) {
        const auto sc = detail::ssrConstants(proj, *sceneCamera(), post);
        drawPostPass(m_ssrScene, m_progSsr, postSourceTex(), linear, m_aoDepth.tex, linear, &sc, sizeof(sc));
        m_ssrThisFrame = true;
        m_postCurrent = &m_ssrScene;
    }
    if (sun) {
        const auto sc = detail::shaftsConstants(proj, *sceneCamera(), sceneSun()->direction, sceneSun()->color, post);
        if (sc.a[2] > 0.0f && sc.a[3] > 0.0f) {
            drawPostPass(m_shaftScene, m_progShafts, postSourceTex(), linear, m_aoDepth.tex, linear, &sc, sizeof(sc));
            m_postCurrent = &m_shaftScene;
        }
    }
    if (moving) {
        const auto mc = detail::motionBlurConstants(proj, *sceneCamera(), *m_prevProjection, *m_prevCamera, post,
                                                    post.time - m_prevTime);
        if (mc.params[3] > 0.0f) {
            drawPostPass(m_blurScene, m_progBlur, postSourceTex(), linear, m_aoDepth.tex, linear, &mc, sizeof(mc));
            m_postCurrent = &m_blurScene;
        }
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlDevice::runBloom() {
    const PostProcess& post = postProcess();
    m_bloomThisFrame = false;
    if (!m_sceneRendered || post.bloom <= 0.0f || !m_bloomUp[0].fbo)
        return;
    beginPostPasses();
    const GLuint linear = sampler({Filter::Bilinear, AddressMode::Clamp, AddressMode::Clamp});
    auto pass = [&](const PostTarget& dst, GLuint src, std::uint32_t srcW, std::uint32_t srcH, const PostTarget* add,
                    float mode) {
        detail::GpuBloomConstants bc{};
        bc.params[0] = mode;
        bc.params[1] = post.bloomThreshold;
        bc.texel[0] = 1.0f / static_cast<float>(srcW);
        bc.texel[1] = 1.0f / static_cast<float>(srcH);
        if (add) {
            bc.texel[2] = 1.0f / static_cast<float>(add->width);
            bc.texel[3] = 1.0f / static_cast<float>(add->height);
        }
        drawPostPass(dst, m_progBloom, src, linear, add ? add->tex : 0, linear, &bc, sizeof(bc));
    };
    // Down: the bright pass from the scene, then halving. Up: each level's
    // down image mixed with the blurred next smaller level.
    pass(m_bloomDown[0], postSourceTex(), m_sceneExtent.width, m_sceneExtent.height, nullptr, 0.0f);
    for (std::size_t i = 1; i < m_bloomDown.size(); ++i)
        pass(m_bloomDown[i], m_bloomDown[i - 1].tex, m_bloomDown[i - 1].width, m_bloomDown[i - 1].height, nullptr,
             1.0f);
    const std::size_t last = m_bloomUp.size() - 1;
    pass(m_bloomUp[last], m_bloomDown[last].tex, m_bloomDown[last].width, m_bloomDown[last].height,
         &m_bloomDown[last + 1], 2.0f);
    for (std::size_t i = last; i-- > 0;)
        pass(m_bloomUp[i], m_bloomDown[i].tex, m_bloomDown[i].width, m_bloomDown[i].height, &m_bloomUp[i + 1], 2.0f);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    m_bloomThisFrame = true;
}

// --- Frame -----------------------------------------------------------------------

bool GlDevice::beginFrame() {
    SDL_GL_MakeCurrent(m_window.sdl(), m_context);
    const platform::Extent px = m_window.pixelSize();
    if (m_window.minimized() || px.empty())
        return false;
    const Extent2D out{static_cast<std::uint32_t>(px.width), static_cast<std::uint32_t>(px.height)};
    if (out != m_output || m_resizePending || !m_sceneFbo) {
        m_output = out;
        createSceneTargets();
        m_resizePending = false;
    }
    resetRing(m_vertexRing);
    resetRing(m_indexRing);
    resetRing(m_uniformRing);
    m_stats = {};
    m_stats.apiErrors = static_cast<std::uint32_t>(m_debugErrors);
    m_inFrame = true;
    m_sceneRendered = false;
    m_shadowThisFrame = false;
    m_boundShadowTex = ~0u;
    m_captureReady = false;
    invalidateState();
    return true;
}

void GlDevice::setDefaultViewport() {
    const Extent2D e = m_pass == Pass::Scene    ? m_sceneExtent
                       : m_pass == Pass::Shadow ? Extent2D{m_shadowSize, m_shadowSize}
                                                : m_output;
    setViewport({0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1});
    setScissor(nullptr);
}

void GlDevice::beginScene(const ClearValues& clearValues) {
    m_pass = Pass::Scene;
    gl.BindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
    setDefaultViewport();
    clear(clearValues);
}

void GlDevice::endScene() {
    if (m_samples > 1) {
        const auto w = static_cast<GLint>(m_sceneExtent.width), h = static_cast<GLint>(m_sceneExtent.height);
        gl.Disable(GL_SCISSOR_TEST);
        m_scissorOn = false;
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFbo);
        gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    m_sceneRendered = true;
    m_pass = Pass::None;
}

bool GlDevice::beginShadowPass(std::uint32_t size) {
    if (!m_inFrame || m_pass != Pass::None || size == 0)
        return false;
    if (m_shadowSize != size || !m_shadowFbo) {
        if (m_shadowFbo)
            gl.DeleteFramebuffers(1, &m_shadowFbo);
        if (m_shadowTex)
            gl.DeleteTextures(1, &m_shadowTex);
        gl.GenTextures(1, &m_shadowTex);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, m_shadowTex);
        m_boundTex[0] = ~0u;
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, static_cast<GLsizei>(size), static_cast<GLsizei>(size), 0,
                      GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        gl.GenFramebuffers(1, &m_shadowFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowTex, 0);
        gl.DrawBuffer(GL_NONE);
        gl.ReadBuffer(GL_NONE);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log::error("gl: shadow framebuffer incomplete");
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        m_shadowSize = size;
        m_boundShadowTex = ~0u;
    }
    m_pass = Pass::Shadow;
    m_shadowThisFrame = false; // the map is being written: draws bind white
    gl.BindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
    setDefaultViewport();
    ClearValues cv;
    cv.clearColor = false;
    cv.clearDepth = true;
    cv.depth = 1.0f;
    clear(cv);
    return true;
}

void GlDevice::endShadowPass() {
    if (m_pass != Pass::Shadow)
        return;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    m_pass = Pass::None;
    m_shadowThisFrame = true;
    m_boundShadowTex = ~0u;
}

void GlDevice::beginOverlay(const Vec4& clearColor) {
    runAo(); // OpenMM2 post-processing, before the composite
    runBloom();
    m_pass = Pass::Overlay;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    setDefaultViewport();
    ClearValues cv;
    cv.color = clearColor;
    cv.clearDepth = false;
    clear(cv);
    if (m_sceneRendered) {
        gl.Disable(GL_BLEND);
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_CULL_FACE);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        invalidateState();
        gl.UseProgram(m_progComposite);
        m_currentProgram = m_progComposite;
        gl.BindVertexArray(m_vaoEmpty);
        m_currentVao = m_vaoEmpty;
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, postSourceTex());
        m_boundTex[0] = ~0u;
        const Filter f = m_sceneExtent == m_output ? Filter::Point : Filter::Bilinear;
        const GLuint s = sampler({f, AddressMode::Clamp, AddressMode::Clamp});
        gl.BindSampler(0, s);
        m_boundSampler[0] = s;
        // Unit 1: the bloom (composite.frag reads it only with bloom on).
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, m_bloomThisFrame ? m_bloomUp[0].tex : m_sceneTex);
        const GLuint s1 = sampler({Filter::Bilinear, AddressMode::Clamp, AddressMode::Clamp});
        gl.BindSampler(1, s1);
        m_boundTex[1] = ~0u;
        m_boundSampler[1] = s1;
        gl.ActiveTexture(GL_TEXTURE0);
        // OpenMM2 image filters (composite.frag's DrawBlock).
        detail::GpuPostConstants pc = detail::toGpu(postProcess());
        if (!m_bloomThisFrame)
            pc.detail[3] = 0.0f;
        detail::setUnfilteredRects(pc, unfilteredRects(), m_sceneExtent, true);
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, m_sceneTex);
        gl.BindSampler(2, sampler({Filter::Bilinear, AddressMode::Clamp, AddressMode::Clamp}));
        gl.ActiveTexture(GL_TEXTURE0);
        m_boundShadowTex = ~0u; // the next mesh draw binds its own unit 2
        const auto [ubuf, uoff] = ringAlloc(m_uniformRing, sizeof(pc), static_cast<std::size_t>(m_uboAlign), nullptr);
        gl.BindBuffer(GL_COPY_WRITE_BUFFER, ubuf);
        gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(uoff), sizeof(pc), &pc);
        gl.BindBufferRange(GL_UNIFORM_BUFFER, 1, ubuf, static_cast<GLintptr>(uoff), sizeof(pc));
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }
}

void GlDevice::endOverlay() { m_pass = Pass::None; }

void GlDevice::endFrame() {
    if (m_captureRequested) {
        const auto w = static_cast<GLsizei>(m_output.width), h = static_cast<GLsizei>(m_output.height);
        m_capture.width = m_output.width;
        m_capture.height = m_output.height;
        m_capture.pixels.resize(static_cast<std::size_t>(w) * h * 4);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl.ReadBuffer(GL_BACK);
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_capture.pixels.data());
        flipVertical(m_capture);
        for (std::size_t i = 3; i < m_capture.pixels.size(); i += 4)
            m_capture.pixels[i] = 255;
        m_captureRequested = false;
        m_captureReady = true;
    }
    SDL_GL_SwapWindow(m_window.sdl());
    m_inFrame = false;
    if (GLenum e = gl.GetError(); e != GL_NO_ERROR) {
        log::error("gl: error 0x{:04x} during frame", e);
        ++m_debugErrors;
    }
    m_stats.apiErrors = static_cast<std::uint32_t>(m_debugErrors);
}

bool GlDevice::readCapture(Image& out) {
    if (!m_captureReady)
        return false;
    out = std::move(m_capture);
    m_captureReady = false;
    return true;
}

// --- Pass commands --------------------------------------------------------------

void GlDevice::setViewport(const Viewport& v) {
    m_viewport = v;
    const auto th = static_cast<float>(targetHeight());
    gl.Viewport(static_cast<GLint>(v.x), static_cast<GLint>(th - (v.y + v.height)), static_cast<GLsizei>(v.width),
                static_cast<GLsizei>(v.height));
    gl.DepthRange(v.minDepth, v.maxDepth);
}

void GlDevice::setScissor(const Rect* r) {
    if (!r) {
        if (m_scissorOn)
            gl.Disable(GL_SCISSOR_TEST);
        m_scissorOn = false;
        return;
    }
    if (!m_scissorOn)
        gl.Enable(GL_SCISSOR_TEST);
    m_scissorOn = true;
    m_scissor = *r;
    const auto th = static_cast<GLint>(targetHeight());
    gl.Scissor(r->x, th - (r->y + static_cast<GLint>(r->height)), static_cast<GLsizei>(r->width),
               static_cast<GLsizei>(r->height));
}

void GlDevice::clear(const ClearValues& cv) {
    GLbitfield mask = 0;
    if (cv.clearColor) {
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.ClearColor(cv.color.x, cv.color.y, cv.color.z, cv.color.w);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (cv.clearDepth && (m_pass == Pass::Scene || m_pass == Pass::Shadow)) {
        gl.DepthMask(GL_TRUE);
        gl.ClearDepth(cv.depth);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (!mask)
        return;
    // Clear only the current viewport (glClear honours the scissor box).
    const bool hadScissor = m_scissorOn;
    const Rect saved = m_scissor;
    const Rect vp{static_cast<std::int32_t>(m_viewport.x), static_cast<std::int32_t>(m_viewport.y),
                  static_cast<std::uint32_t>(m_viewport.width), static_cast<std::uint32_t>(m_viewport.height)};
    setScissor(&vp);
    gl.Clear(mask);
    if (hadScissor)
        setScissor(&saved);
    else
        setScissor(nullptr);
    invalidateState();
}

void GlDevice::setFrameConstants(const FrameConstants& fc) {
    const detail::GpuFrameConstants g = detail::toGpu(fc);
    const auto [buf, offset] = ringAlloc(m_uniformRing, sizeof(g), static_cast<std::size_t>(m_uboAlign), nullptr);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, buf);
    gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), sizeof(g), &g);
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 0, buf, static_cast<GLintptr>(offset), sizeof(g));
}

void GlDevice::applyState(const PipelineState& s, const DrawCall& call) {
    const bool force = !m_stateValid;
    const PipelineState& o = m_state;
    if (force || s.blend != o.blend) {
        if (s.blend == BlendMode::Opaque) {
            gl.Disable(GL_BLEND);
        } else {
            gl.Enable(GL_BLEND);
            gl.BlendEquation(GL_FUNC_ADD);
            switch (s.blend) {
            case BlendMode::Alpha: gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Additive: gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE); break;
            case BlendMode::Modulate: gl.BlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE); break;
            case BlendMode::Premultiplied: gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Add: gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE); break;
            case BlendMode::Opaque: break;
            }
        }
    }
    if (force || s.cull != o.cull) {
        if (s.cull == CullMode::None) {
            gl.Disable(GL_CULL_FACE);
        } else {
            gl.Enable(GL_CULL_FACE);
            gl.CullFace(s.cull == CullMode::Back ? GL_BACK : GL_FRONT);
        }
    }
    if (force || s.frontFace != o.frontFace)
        gl.FrontFace(s.frontFace == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
    if (force || s.depthTest != o.depthTest || s.depthCompare != o.depthCompare) {
        if (s.depthTest) {
            gl.Enable(GL_DEPTH_TEST);
            gl.DepthFunc(toGl(s.depthCompare));
        } else {
            gl.Disable(GL_DEPTH_TEST);
        }
    }
    if (force || s.depthWrite != o.depthWrite)
        gl.DepthMask(s.depthWrite ? GL_TRUE : GL_FALSE);
    if (force || s.colorWrite != o.colorWrite) {
        const GLboolean c = s.colorWrite ? GL_TRUE : GL_FALSE;
        gl.ColorMask(c, c, c, c);
    }
    if (s.depthBias) {
        gl.Enable(GL_POLYGON_OFFSET_FILL);
        gl.PolygonOffset(-call.depthBiasSlope, -call.depthBias);
    } else if (force || o.depthBias) {
        gl.Disable(GL_POLYGON_OFFSET_FILL);
    }
    if (force || s.vertexFormat != o.vertexFormat || m_currentProgram == m_progComposite) {
        const GLuint prog = s.vertexFormat == VertexFormat::Mesh ? m_progMesh : m_progOverlay;
        gl.UseProgram(prog);
        m_currentProgram = prog;
        ++m_stats.pipelineBinds;
    }
    m_state = s;
    m_stateValid = true;
}

void GlDevice::draw(const DrawCall& drawn) {
    if (drawn.count == 0)
        return;
    // OpenMM2 sun shadows: the shadow pass draws casters depth only.
    std::optional<DrawCall> caster;
    if (m_pass == Pass::Shadow) {
        caster = shadowCasterCall(drawn);
        if (!caster)
            return;
    }
    const DrawCall& call = caster ? *caster : drawn;
    const GlBuffer* vb = m_buffers.get(call.vertices.buffer.id);
    if (!vb)
        return;
    const GlBuffer* ib = call.indices ? m_buffers.get(call.indices.buffer.id) : nullptr;
    if (call.indices && !ib)
        return;
    const PipelineState s = effectiveState(call.state);
    applyState(s, call);

    // Vertex input.
    const int fmt = s.vertexFormat == VertexFormat::Mesh ? 0 : 1;
    const GLuint vao = fmt == 0 ? m_vaoMesh : m_vaoOverlay;
    if (m_currentVao != vao) {
        gl.BindVertexArray(vao);
        m_currentVao = vao;
    }
    VaoBinding& vs = m_vaoState[fmt];
    if (vs.vb != vb->id || vs.offset != call.vertices.offset) {
        gl.BindBuffer(GL_ARRAY_BUFFER, vb->id);
        auto at = [&](std::size_t off) {
            return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(call.vertices.offset + off));
        };
        if (fmt == 0) {
            constexpr GLsizei stride = sizeof(Vertex3D);
            gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex3D, position)));
            gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex3D, normal)));
            gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(offsetof(Vertex3D, color)));
            gl.VertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex3D, uv0)));
            gl.VertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex3D, uv1)));
        } else {
            constexpr GLsizei stride = sizeof(Vertex2D);
            gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex2D, position)));
            gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex2D, uv)));
            gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(offsetof(Vertex2D, color)));
        }
        vs.vb = vb->id;
        vs.offset = call.vertices.offset;
    }
    if (ib && vs.ib != ib->id) {
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->id);
        vs.ib = ib->id;
    }

    // Textures.
    for (int unit = 0; unit < 2; ++unit) {
        const TextureBinding& tb = call.textures[static_cast<std::size_t>(unit)];
        const GlTexture* t = m_textures.get(tb.texture.id);
        const GLuint id = t ? t->id : m_white;
        if (m_boundTex[unit] != id) {
            gl.ActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            gl.BindTexture(GL_TEXTURE_2D, id);
            m_boundTex[unit] = id;
        }
        const GLuint smp = sampler(tb.sampler);
        if (m_boundSampler[unit] != smp) {
            gl.BindSampler(static_cast<GLuint>(unit), smp);
            m_boundSampler[unit] = smp;
        }
    }

    // Unit 2 of mesh draws: the shadow map in a scene pass after this
    // frame's shadow pass, white otherwise.
    if (s.vertexFormat == VertexFormat::Mesh) {
        const bool map = m_pass == Pass::Scene && m_shadowThisFrame && m_shadowTex;
        const GLuint tex = map ? m_shadowTex : m_white;
        if (m_boundShadowTex != tex) {
            gl.ActiveTexture(GL_TEXTURE2);
            gl.BindTexture(GL_TEXTURE_2D, tex);
            gl.BindSampler(2, sampler({Filter::Point, AddressMode::Clamp, AddressMode::Clamp}));
            gl.ActiveTexture(GL_TEXTURE0);
            m_boundTex[0] = ~0u;
            m_boundShadowTex = tex;
        }
    }

    // Per-draw constants.
    detail::GpuDrawConstants dc = detail::toGpu(call.constants);
    if (m_pass == Pass::Scene)
        dc.flags = sceneFlags(call);
    const auto [ubuf, uoff] = ringAlloc(m_uniformRing, sizeof(dc), static_cast<std::size_t>(m_uboAlign), nullptr);
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, ubuf);
    gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(uoff), sizeof(dc), &dc);
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 1, ubuf, static_cast<GLintptr>(uoff), sizeof(dc));

    const GLenum mode = toGl(s.topology);
    if (ib) {
        const std::size_t isz = call.indexType == IndexType::U16 ? 2 : 4;
        const auto* ptr = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(call.indices.offset + static_cast<std::size_t>(call.first) * isz));
        gl.DrawElementsBaseVertex(mode, static_cast<GLsizei>(call.count),
                                  call.indexType == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, ptr,
                                  call.baseVertex);
    } else {
        gl.DrawArrays(mode, static_cast<GLint>(call.first), static_cast<GLsizei>(call.count));
    }
    ++m_stats.drawCalls;
    m_stats.primitives += s.topology == Topology::LineList        ? call.count / 2
                          : s.topology == Topology::TriangleStrip ? (call.count > 2 ? call.count - 2 : 0)
                                                                  : call.count / 3;
}

} // namespace

std::unique_ptr<Device> createOpenGLDevice(const DeviceCreateInfo& info, std::string* error) {
    if (!info.window || info.window->api() != platform::GraphicsApi::OpenGL) {
        *error = "window was not created for OpenGL";
        return nullptr;
    }
    auto dev = std::make_unique<GlDevice>(*info.window);
    if (!dev->init(info, error))
        return nullptr;
    return dev;
}

} // namespace mm2::render
