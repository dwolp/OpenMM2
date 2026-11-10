#pragma once

#include "asset/Pkg.h"
#include "game/ModelLibrary.h"
#include "game/TextureLibrary.h"
#include "render/Device.h"

#include <optional>
#include <vector>

namespace mm2::game {

struct MeshDrawOptions {
    bool fog = true;
    bool lighting = true;
    // Overrides the blend of every draw (e.g. shadows: Alpha; glows: Add).
    std::optional<render::BlendMode> blend;
    bool depthWrite = true;
    bool depthBias = false; // pull towards the viewer (decals, shadows)
    // Alpha test reference for translucent materials (which also blend).
    // cityLevel::DrawRooms sets GREATER 100 for everything it draws.
    float alphaRef = 101.0f / 255.0f;
    Vec4 tint{1, 1, 1, 1}; // multiplied into the material colour
    // Back-face culling with counter-clockwise front faces, as the original
    // drew (verified: car bodies are correct only this way, and models ship
    // explicit reversed copies of double-sided faces such as banners).
    bool cull = true;
    float dither = 0.0f; // OpenMM2 fades (render::DrawConstants::dither)
    // OpenMM2 enhancement (tweak EnhancedCarLighting, app/Tweaks.h; not in MM2): lit draws
    // are lit per pixel with a highlight scaled by the material's shininess
    // (render::DrawFlag::PixelLighting).
    bool pixelLighting = false;
    // OpenMM2 clear coat (pixelLighting; 0 = none): the sky's reflection and
    // sun glint on materials that shine (render::DrawConstants::emissive.w).
    float gloss = 0.0f;
    // OpenMM2 live reflections: the clear coat mirrors this reflection probe
    // (render::Device::probeTexture, DrawFlag::EnvProbe) instead of the sky;
    // null: the sky.
    render::TextureHandle probe;
    // OpenMM2 ray traced reflections: the clear coat traces its reflection
    // (DrawFlag::EnvProbe without a probe).
    bool rayReflections = false;
    // OpenMM2 grass: draws with a grass texture get render::DrawFlag::Grass
    // and, with layers, that many shell layers after them (render::
    // DrawFlag::GrassShell; CityRenderer's grass on the city's own models).
    bool grass = false;
    int grassLayers = 0;
};

// The enhanced lighting's highlight: the material's shininess (MM2's "power",
// 0.5 on car paint and 0 on tyres and undersides, which MM2 used only for the
// reflection) times this, with this Blinn-Phong exponent. Chosen by eye.
inline constexpr float kHighlightScale = 0.8f;
inline constexpr float kHighlightPower = 48.0f;
// How the reflection probe is sampled: bilinear, held inside the atlas.
inline constexpr render::SamplerDesc kProbeSampler{render::Filter::Bilinear, render::AddressMode::Clamp,
                                                   render::AddressMode::Clamp};


// Draws a PKG mesh with a paint job's materials. Returns the number of draw calls.
int drawGpuMesh(render::Device& device, TextureLibrary& textures, const GpuMesh& mesh,
                const std::vector<asset::PkgMaterial>& materials, const Mat44& world, const MeshDrawOptions& options = {});

// lvlInstance's level-of-detail thresholds in metres, set by the Object
// Detail option (cityLevel::SetObjectDetail, levels 0-3; level 2 is also
// the static default).
struct ObjectDetail {
    float med = 40.0f, low = 100.0f, vlow = 200.0f, noDraw = 300.0f;
    static ObjectDetail forLevel(int level);
};

// lvlInstance::IsVisible: with `limit` (sm_ObjMaxThresh: NoDraw for dynamic
// objects, none for the city's static ones) an object whose centre is
// deeper than it is not drawn. Then d = view depth - radius picks the very
// low mesh beyond VLow, the low one beyond Low, the medium one beyond Med,
// else the high one.
std::optional<asset::Lod> objectLod(float depth, float radius, const ObjectDetail& detail,
                                    std::optional<float> limit = std::nullopt);

// OpenMM2 fades (not in MM2): objectLod as a crossfade. Within a band about
// each of its thresholds (5% of it and 2 m either side) the levels on both
// sides are drawn, dithered between them by how far across the band the
// object is (render::DrawConstants::dither): `lod` on `share` of the
// pixels, `other` on the rest; over the last 10% before `limit` the object
// thins out (`share` alone). Outside them, `lod` with share 1, as objectLod.
// OpenMM2 fades: a dither for the draws that set none of their own
// (MeshDrawOptions::dither 0), while a whole object (a car of many parts)
// thins out. Set it around the object's draws and back to 0 after.
void setDrawDither(float dither);
float drawDither();
// The share of an object `depth` deep drawn as it nears `limit` (the last
// 10% of it thins out; 1 before).
float limitShare(float depth, float limit);

struct LodFade {
    asset::Lod lod = asset::Lod::High;
    float share = 1.0f;
    std::optional<asset::Lod> other;
};
std::optional<LodFade> objectLodFade(float depth, float radius, const ObjectDetail& detail,
                                     std::optional<float> limit = std::nullopt);

// View-space depth of `p` for a camera placed at `camera` (looking down -Z).
inline float viewDepth(const Mat34& camera, const Vec3& p) { return -(p - camera.m3).dot(camera.m2); }

} // namespace mm2::game
