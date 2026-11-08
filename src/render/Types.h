#pragma once

// Render hardware interface (RHI) types.
//
// The RHI is shaped after what Midtown Madness 2's Direct3D 7 renderer needs
// (fixed-function style materials, vertex lighting, fog, alpha test, a few
// blend modes, two texture stages) and is implemented on Vulkan and OpenGL.
//
// Conventions shared by both backends:
//   * Matrices are mm2::Mat44 (row-major, row vectors, p' = p * M).
//   * Clip space is Direct3D-like: x right, y up, depth 0 (near) .. 1 (far).
//     Use Mat44::perspective(..., zeroToOne = true).
//   * Pixel/viewport coordinates have their origin at the top-left.
//   * Texture data is uploaded top row first; uv (0,0) is the top-left texel.
//   * Colours are 8-bit gamma-space values and blending happens in gamma
//     space, like the original renderer (no sRGB conversion anywhere).

#include "core/Math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mm2::render {

enum class Backend { Auto, Vulkan, OpenGL };
const char* backendName(Backend b);

// Opaque resource handles. 0 is the null handle.
struct TextureHandle {
    std::uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const TextureHandle&) const = default;
};
struct BufferHandle {
    std::uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const BufferHandle&) const = default;
};

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const Extent2D&) const = default;
};

struct Rect {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};

struct Viewport {
    float x = 0, y = 0, width = 0, height = 0;
    float minDepth = 0.0f, maxDepth = 1.0f;
};

// CPU-side RGBA8 image (top row first, tightly packed).
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // width * height * 4
};

// --- Textures ---------------------------------------------------------------

enum class TextureFormat : std::uint8_t {
    RGBA8, // 8-bit unsigned normalized, R in the lowest byte
};

struct TextureDesc {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mipLevels = 1;
    TextureFormat format = TextureFormat::RGBA8;
    std::string debugName;
};

// Pixel data for one mip level (tightly packed rows unless rowPitch is set).
struct TextureData {
    const void* data = nullptr;
    std::uint32_t rowPitch = 0; // bytes; 0 = width * 4
};

enum class Filter : std::uint8_t {
    Point,     // nearest, no mip filtering
    Bilinear,  // linear, nearest mip
    Trilinear, // linear, linear mip; uses the global anisotropy setting
};

enum class AddressMode : std::uint8_t { Wrap, Clamp, Mirror };

struct SamplerDesc {
    Filter filter = Filter::Trilinear;
    AddressMode addressU = AddressMode::Wrap;
    AddressMode addressV = AddressMode::Wrap;
    bool operator==(const SamplerDesc&) const = default;
    std::uint32_t key() const {
        return static_cast<std::uint32_t>(filter) | (static_cast<std::uint32_t>(addressU) << 4) |
               (static_cast<std::uint32_t>(addressV) << 8);
    }
};

struct TextureBinding {
    TextureHandle texture; // null = white 1x1
    SamplerDesc sampler;
};

// --- Buffers ----------------------------------------------------------------

enum class BufferKind : std::uint8_t { Vertex, Index };

// A range inside a buffer. For transient data, `buffer` refers to an
// internal per-frame ring buffer and the slice is valid for the current frame.
struct BufferSlice {
    BufferHandle buffer;
    std::uint32_t offset = 0; // bytes
    explicit operator bool() const { return static_cast<bool>(buffer); }
};

enum class IndexType : std::uint8_t { U16, U32 };

// --- Vertex formats ---------------------------------------------------------

enum class VertexFormat : std::uint8_t {
    Mesh,    // Vertex3D: world geometry, models
    Overlay, // Vertex2D: UI, HUD, ImGui (layout identical to ImDrawVert)
};

struct Vertex3D {
    float position[3];
    float normal[3];
    std::uint32_t color; // RGBA8, R in the lowest byte (0xAABBGGRR)
    float uv0[2];
    float uv1[2];
};
static_assert(sizeof(Vertex3D) == 44);

struct Vertex2D {
    float position[2]; // output pixels (overlay pass) or any space mapped by FrameConstants::proj
    float uv[2];
    std::uint32_t color; // RGBA8
};
static_assert(sizeof(Vertex2D) == 20);

constexpr std::uint32_t packColor(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(b) << 16) |
           (static_cast<std::uint32_t>(a) << 24);
}
constexpr std::uint32_t packColor(const Vec4& c) {
    auto u8 = [](float v) { return static_cast<std::uint8_t>(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return packColor(u8(c.x), u8(c.y), u8(c.z), u8(c.w));
}

// --- Pipeline state -------------------------------------------------------

enum class BlendMode : std::uint8_t {
    Opaque,        // no blending
    Alpha,         // src*a + dst*(1-a)
    Additive,      // src*a + dst
    Modulate,      // src * dst (multiplicative, e.g. shadows/lightmaps)
    Premultiplied, // src + dst*(1-a)
    Add,           // src + dst (Direct3D ONE/ONE: light glows)
};

enum class CullMode : std::uint8_t { None, Back, Front };
enum class FrontFace : std::uint8_t { CounterClockwise, Clockwise };
enum class CompareOp : std::uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Topology : std::uint8_t { TriangleList, TriangleStrip, LineList };

// Fixed-function state that maps to a pipeline object on Vulkan.
struct PipelineState {
    VertexFormat vertexFormat = VertexFormat::Mesh;
    Topology topology = Topology::TriangleList;
    BlendMode blend = BlendMode::Opaque;
    CullMode cull = CullMode::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompare = CompareOp::LessEqual;
    bool depthBias = false; // uses DrawCall::depthBias (decals, shadows)
    bool colorWrite = true;

    bool operator==(const PipelineState&) const = default;
    std::uint32_t key() const;
};

// --- Shader constants -------------------------------------------------------

enum class FogMode : std::uint8_t { None, Linear, Exp, Exp2 };

struct DirectionalLight {
    Vec3 direction{0, -1, 0}; // direction the light travels (D3D convention)
    Vec3 color{0, 0, 0};
};

// Set per view/pass with Device::setFrameConstants().
struct FrameConstants {
    Mat44 view;
    Mat44 proj;
    Vec3 cameraPosition;
    FogMode fogMode = FogMode::None;
    Vec3 fogColor{0.5f, 0.5f, 0.5f};
    float fogStart = 0.0f;
    float fogEnd = 1000.0f;
    float fogDensity = 0.0f;
    Vec3 ambient{0.3f, 0.3f, 0.3f};
    std::array<DirectionalLight, 3> lights{};
};

// Per-draw feature switches (DrawConstants::flags).
namespace DrawFlag {
inline constexpr std::uint32_t Texture0 = 1u << 0;     // sample stage 0 (uv0)
inline constexpr std::uint32_t Texture1 = 1u << 1;     // sample stage 1 (uv1, or env map)
inline constexpr std::uint32_t AlphaTest = 1u << 2;    // discard if alpha < alphaRef
inline constexpr std::uint32_t Fog = 1u << 3;          // apply frame fog
inline constexpr std::uint32_t Lighting = 1u << 4;     // vertex lighting with frame lights
inline constexpr std::uint32_t VertexColor = 1u << 5;  // multiply by vertex colour
inline constexpr std::uint32_t EnvMap1 = 1u << 6;      // stage 1 uses sphere-map coords from the normal
inline constexpr std::uint32_t Tex1Add = 1u << 7;      // stage 1 combine: add (default: modulate)
inline constexpr std::uint32_t Tex1Modulate2x = 1u << 8; // stage 1 combine: modulate * 2
inline constexpr std::uint32_t Tex1Blend = 1u << 9;    // stage 1 combine: lerp by stage-1 alpha (reflections)
// OpenMM2 enhancement (tweak EnhancedCarLighting, off by default; not in
// MM2): Lighting is evaluated per pixel with renormalised normals and adds a
// highlight from light 0 (DrawConstants::specular); with EnvMap1 the sphere
// map is looked up per pixel and scaled by a Fresnel term.
inline constexpr std::uint32_t PixelLighting = 1u << 10;
} // namespace DrawFlag

struct DrawConstants {
    Mat44 world;
    Vec4 color{1, 1, 1, 1}; // material colour, multiplied in
    float alphaRef = 0.5f;
    std::uint32_t flags = DrawFlag::Texture0 | DrawFlag::VertexColor;
    // DrawFlag::PixelLighting: the highlight's strength and Blinn-Phong exponent.
    float specular = 0.0f;
    float specularPower = 0.0f;
    Vec4 emissive{0, 0, 0, 0}; // added to lit colours (DrawFlag::Lighting)
};

// --- Draw calls ---------------------------------------------------------------

struct DrawCall {
    PipelineState state;
    BufferSlice vertices;
    BufferSlice indices; // null = non-indexed
    IndexType indexType = IndexType::U16;
    std::uint32_t count = 0;      // indices (indexed) or vertices
    std::uint32_t first = 0;      // first index or first vertex
    std::int32_t baseVertex = 0;  // added to indices (indexed only)
    std::array<TextureBinding, 2> textures{};
    DrawConstants constants;
    // Pulls geometry towards the viewer (road decals, shadows); needs
    // state.depthBias. `depthBias` is in units of the smallest resolvable
    // depth step, `depthBiasSlope` scales with the polygon's depth slope.
    float depthBias = 0.0f;
    float depthBiasSlope = 0.0f;
};

// OpenMM2 image filters applied when the scene is composited onto the
// window (shaders/composite.frag; not in MM2). The defaults change nothing.
struct PostProcess {
    float contrast = 1.0f;   // around mid grey
    float saturation = 1.0f; // 0 = grey
    float warmth = 0.0f;     // -1 cool .. 1 warm
    float vignette = 0.0f;   // edge darkening, 0..1
    float sharpen = 0.0f;    // contrast-adaptive sharpening, 0..1
    bool fxaa = false;       // fast approximate anti-aliasing
    bool active() const {
        return contrast != 1.0f || saturation != 1.0f || warmth != 0.0f || vignette != 0.0f || sharpen != 0.0f ||
               fxaa;
    }
};

struct ClearValues {
    Vec4 color{0, 0, 0, 1};
    float depth = 1.0f;
    bool clearColor = true;
    bool clearDepth = true;
};

// --- Device information -----------------------------------------------------

struct DeviceInfo {
    Backend backend = Backend::Auto;
    std::string apiVersion;   // "Vulkan 1.3.290" / "OpenGL 4.6 (Core Profile) Mesa ..."
    std::string deviceName;   // GPU name
    std::string driverInfo;
    float maxAnisotropy = 1.0f;
    std::vector<std::uint32_t> msaaSamples; // supported sample counts, e.g. {1,2,4,8}
    std::uint32_t maxTextureSize = 0;
};

} // namespace mm2::render
