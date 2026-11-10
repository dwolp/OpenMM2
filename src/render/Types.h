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
    // OpenMM2 grass shells: src * dst alpha + dst * (1 - a), dst alpha kept.
    // The grass under them wrote its sun shade into the alpha (mesh.frag),
    // so the 16 layers take the shade without each finding it again; the
    // shader gives them premultiplied colour.
    Shaded,
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

// OpenMM2 night lighting (not in MM2): a point or spot light that lights
// the scene's solid draws (mesh.frag's sceneLights): street lamps,
// headlights, sirens.
struct SceneLight {
    Vec3 position;
    float range = 10.0f;         // no light past this distance (m)
    Vec3 direction{0, 0, -1};    // spot lights: where they point
    float cosOuter = -2.0f;      // the cone's edge (cosine); below -1: a point light
    Vec3 color{1, 1, 1};         // may be brighter than 1
    float cosInner = 1.0f;       // full strength inside this cosine
};
inline constexpr std::size_t kMaxSceneLights = 48;

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
    // OpenMM2 sun shadows (Device::beginShadowPass; not in MM2): up to
    // kShadowCascades views along the sun, nearest first, drawn into the
    // shadow map's 2 x 2 cells (cascade i at column i % 2, row i / 2, row 0
    // at the top as a viewport places it), each with the world size of one
    // of its texels (the receivers' normal offset and filter) and the depth
    // it spans in metres; how much the shadows darken (0 = none); the map's
    // size in texels (the whole atlas); how much the edges soften with the
    // distance from what casts them (the sun's spread, tan of the angle; 0:
    // evenly soft).
    static constexpr std::size_t kShadowCascades = 4;
    std::array<Mat44, kShadowCascades> shadowCascades{};
    std::array<float, kShadowCascades> shadowTexels{};
    std::array<float, kShadowCascades> shadowDepths{};
    std::uint32_t shadowCascadeCount = 0;
    float shadowStrength = 0.0f;
    float shadowSize = 0.0f;
    float shadowPenumbra = 0.0f;
    // OpenMM2 ray traced sun shadows (Device::beginRayScene): the scene's
    // draws trace a ray towards the sun through this frame's ray scene
    // instead of reading the shadow map (strength as shadowStrength).
    bool shadowRayTraced = false;
    // OpenMM2 ray traced reflections: the draws with DrawFlag::EnvProbe
    // trace their reflection through the ray scene instead of reading the
    // probe.
    bool rayReflections = false;
    // OpenMM2 ray traced night lights' shadows: the scene lights trace a
    // ray to each that lights a point (the first eight).
    bool rayLightShadows = false;
    // OpenMM2 glowing windows (not in MM2): the bright texels of
    // DrawFlag::NightLit draws shine at this strength, whatever their baked
    // light (0: as the game draws them).
    float windowGlow = 0.0f;
    // OpenMM2 fades: a number that changes every frame, so the dither's
    // pattern moves and temporal anti-aliasing averages it (DrawConstants::
    // dither).
    float ditherPhase = 0.0f;
    // OpenMM2 water (not in MM2): DrawFlag::Water draws get moving waves, a
    // Fresnel mirror and the sun's glint. 0: as the game draws them; 1: the
    // mirror shows the sky; 2: it traces the ray scene (rayReflections).
    int water = 0;
    // OpenMM2 drifting cloud shadows (not in MM2), with the sun shadows: how
    // much of the sky the clouds cover (0: none) and how much sun they take
    // where they are (0..1). They drift with ground.w's time.
    float cloudCover = 0.0f;
    float cloudStrength = 0.0f;
    // OpenMM2: the wet roads' wetness when ray traced (0: none; the
    // reflections' rays, else the screen-space pass), and for the grass
    // (DrawFlag::Grass) the blades' height in metres (0 = no grass), the
    // distance they reach; the time in seconds (their sway, the ripples).
    Vec4 ground{0, 0, 30, 0};
    // OpenMM2 distance haze (not in MM2; solid draws, before the game's own
    // fog): density per metre (0 = none), height falloff per metre, the
    // glow towards the sun, the most it may cover (0..1).
    Vec4 haze{0, 0, 0, 0};
    // OpenMM2 night lighting: the first sceneLightCount lights.
    std::array<SceneLight, kMaxSceneLights> sceneLights{};
    std::uint32_t sceneLightCount = 0;
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
// OpenMM2 live reflections (tweak LiveReflections; not in MM2): stage 1 is
// the reflection probe (Device::probeTexture, six 90-degree faces in a 3 x 2
// atlas, game/ReflectionProbe.h), looked up by the reflected view
// direction: the EnvMap1 reflection and the clear coat read it instead of
// the sphere map and the sky; with FrameConstants::rayReflections they
// trace it instead.
inline constexpr std::uint32_t EnvProbe = 1u << 11;
// Set by the backends, not by callers: the draw takes sun shadows (scene
// pass draws that are not additive).
inline constexpr std::uint32_t ReceiveShadow = 1u << 12;
// Set by callers: never takes sun shadows (the sky dome, drawn around the
// camera and inside the shadow map's area, would catch the buildings').
inline constexpr std::uint32_t NoShadow = 1u << 13;
// Set by callers: the city's grass, which the OpenMM2 grass tweak
// (FrameConstants::ground) tints and darkens between the blades.
inline constexpr std::uint32_t Grass = 1u << 16;
// Set by callers with Grass: one shell layer of blades, the ground lifted
// by DrawConstants::specular (0..1) of the grass height. Casts no shadow.
inline constexpr std::uint32_t GrassShell = 1u << 15;
// Set by callers: the vertex normals are placeholders (the PSDL street
// geometry's, all up, walls included); the sun shadows take the surface's
// own normal from the screen derivatives instead.
inline constexpr std::uint32_t FlatNormals = 1u << 17;
// Set by callers: texture 0 is a night variant ("<name>_ni": lit windows and
// shop fronts on dark walls), whose bright texels the OpenMM2 glowing
// windows (FrameConstants::windowGlow) make shine.
inline constexpr std::uint32_t NightLit = 1u << 18;
// Set by callers: the city's water (the bay, ponds: a "water" or "deepwater"
// texture in city/materials.csv), which the OpenMM2 water
// (FrameConstants::water) shades with waves and reflections.
inline constexpr std::uint32_t Water = 1u << 19;
} // namespace DrawFlag

struct DrawConstants {
    Mat44 world;
    Vec4 color{1, 1, 1, 1}; // material colour, multiplied in
    float alphaRef = 0.5f;
    std::uint32_t flags = DrawFlag::Texture0 | DrawFlag::VertexColor;
    // DrawFlag::PixelLighting: the highlight's strength and Blinn-Phong exponent.
    // DrawFlag::GrassShell: specular is the layer's height, 0..1.
    float specular = 0.0f;
    float specularPower = 0.0f;
    Vec4 emissive{0, 0, 0, 0}; // added to lit colours (DrawFlag::Lighting)
    // OpenMM2 fades (not in MM2): a screen-door dither, for crossfading a
    // model's levels of detail. 0: none; > 0: the share of pixels drawn;
    // < 0: the other pixels of -dither's share (the two together cover
    // every pixel once). Temporal anti-aliasing smooths it into a blend.
    float dither = 0.0f;
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
    float exposure = 1.0f;   // brightness, applied first
    float contrast = 1.0f;   // around mid grey
    float saturation = 1.0f; // 0 = grey
    float warmth = 0.0f;     // -1 cool .. 1 warm
    float vignette = 0.0f;   // edge darkening, 0..1
    float sharpen = 0.0f;    // contrast-adaptive sharpening, 0..1
    bool fxaa = false;       // fast approximate anti-aliasing
    float bloom = 0.0f;      // glow added around bright areas, 0 = off
    float bloomThreshold = 0.65f; // brightness (luma) where the glow starts
    // Screen-space ambient occlusion: contact shading where surfaces meet.
    // Needs the main camera's projection (Device::setSceneProjection).
    float ao = 0.0f;        // strength, 0 = off
    float aoRadius = 0.8f;  // metres
    float aoFade = 150.0f;  // metres: none beyond
    // Wet roads: screen-space reflections on surfaces facing up, with
    // puddles and rain ripples (ssr.frag). Needs the scene projection and
    // camera (Device::setSceneProjection / setSceneCamera).
    float wetness = 0.0f;   // 0 dry .. 1
    // OpenMM2 heat shimmer (ssr.frag, in the same pass): the air over hot
    // ground far ahead wavers, and at the most grazing angles mirrors the
    // sky (a mirage). 0 = off .. 1.
    float heat = 0.0f;
    float time = 0.0f;      // seconds, for animated effects (not a filter)
    // Sun shafts and glare (shafts.frag), from the sun the race gives
    // (Device::setSceneSun).
    float shafts = 0.0f;    // strength, 0 = off
    // Camera motion blur (motionblur.frag) from this and the previous
    // frame's scene camera; the strength is the exposure in 1/60 s.
    float motionBlur = 0.0f;
    // OpenMM2 temporal anti-aliasing and upscaling (taa.frag): the scene's
    // projection jittered (Device::taaJitter) and blended into the last
    // frames' at the window's resolution.
    bool taa = false;
    bool active() const {
        return exposure != 1.0f || contrast != 1.0f || saturation != 1.0f || warmth != 0.0f || vignette != 0.0f || sharpen != 0.0f ||
               fxaa || bloom > 0.0f || ao > 0.0f || wetness > 0.0f || heat > 0.0f || shafts > 0.0f || motionBlur > 0.0f || taa;
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
