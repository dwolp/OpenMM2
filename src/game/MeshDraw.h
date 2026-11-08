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
    // OpenMM2 enhancement (tweak EnhancedCarLighting, app/Tweaks.h; not in MM2): lit draws
    // are lit per pixel with a highlight scaled by the material's shininess
    // (render::DrawFlag::PixelLighting).
    bool pixelLighting = false;
};

// The enhanced lighting's highlight: the material's shininess (MM2's "power",
// 0.5 on car paint and 0 on tyres and undersides, which MM2 used only for the
// reflection) times this, with this Blinn-Phong exponent. Chosen by eye.
inline constexpr float kHighlightScale = 0.8f;
inline constexpr float kHighlightPower = 48.0f;

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

// View-space depth of `p` for a camera placed at `camera` (looking down -Z).
inline float viewDepth(const Mat34& camera, const Vec3& p) { return -(p - camera.m3).dot(camera.m2); }

} // namespace mm2::game
