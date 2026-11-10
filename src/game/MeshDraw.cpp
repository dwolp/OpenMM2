#include "game/MeshDraw.h"

#include <algorithm>

namespace mm2::game {

int drawGpuMesh(render::Device& device, TextureLibrary& textures, const GpuMesh& mesh,
                const std::vector<asset::PkgMaterial>& materials, const Mat44& world, const MeshDrawOptions& options) {
    int calls = 0;
    for (const auto& d : mesh.draws) {
        const asset::PkgMaterial* mat = d.shader < materials.size() ? &materials[d.shader] : nullptr;
        const WorldTexture* tex = mat && !mat->texture.empty() ? textures.get(mat->texture) : nullptr;
        render::DrawCall call;
        call.vertices = {mesh.vertices, 0};
        call.indices = {mesh.indices, 0};
        call.indexType = render::IndexType::U16;
        call.count = d.indexCount;
        call.first = d.firstIndex;
        call.baseVertex = d.baseVertex;
        call.constants.world = world;
        // Without lighting Direct3D ignores the material: the vertex colour
        // (white for the PKG's colourless vertices) times the texture. This
        // is how MM2 draws pre-lit parts (gfxForceLVERTEX: shadows, light
        // glows), whose materials are black.
        Vec4 diffuse = mat && options.lighting ? mat->diffuse : Vec4{1, 1, 1, 1};
        // At night materials that name no texture are halved too
        // (modShader::Load; a named texture that fails to load is not). The
        // material only counts when lit.
        if (options.lighting && mat && mat->texture.empty() && textures.night())
            diffuse = {diffuse.x * 0.5f, diffuse.y * 0.5f, diffuse.z * 0.5f, diffuse.w};
        call.constants.color = {diffuse.x * options.tint.x, diffuse.y * options.tint.y, diffuse.z * options.tint.z,
                                diffuse.w * options.tint.w};
        // modShader's material emissive colour (the compact form's third
        // colour) is added to the lit colour.
        if (mat && options.lighting)
            call.constants.emissive = mat->emissive;
        std::uint32_t flags = render::DrawFlag::VertexColor;
        if (options.fog)
            flags |= render::DrawFlag::Fog;
        if (options.lighting)
            flags |= render::DrawFlag::Lighting;
        if (options.lighting && options.pixelLighting) {
            flags |= render::DrawFlag::PixelLighting;
            call.constants.specular = mat ? mat->shininess * kHighlightScale : 0.0f;
            call.constants.specularPower = kHighlightPower;
            call.constants.emissive.w = mat && mat->shininess > 0.0f ? options.gloss : 0.0f;
            if (call.constants.emissive.w > 0.0f && (options.probe || options.rayReflections)) {
                flags |= render::DrawFlag::EnvProbe;
                call.textures[1] = {options.probe, kProbeSampler};
            }
        }
        if (tex) {
            flags |= render::DrawFlag::Texture0;
            call.textures[0] = {tex->handle, tex->sampler};
        }
        // modStatic::Draw: a material whose diffuse alpha is not 1 or whose
        // texture format has alpha switches alpha blending on, and the render
        // state ties ALPHATESTENABLE to it.
        const bool translucent = (tex && tex->alphaFormat) || call.constants.color.w < 0.999f;
        if (translucent) {
            flags |= render::DrawFlag::AlphaTest;
            call.constants.alphaRef = options.alphaRef;
            call.state.blend = render::BlendMode::Alpha;
        }
        if (options.blend)
            call.state.blend = *options.blend;
        call.state.depthWrite = options.depthWrite;
        if (options.depthBias) {
            call.state.depthBias = true;
            call.depthBias = 2.0f; // positive pulls towards the viewer (render::DrawCall)
            call.depthBiasSlope = 1.0f;
        }
        call.constants.flags = flags;
        call.state.cull = options.cull ? render::CullMode::Back : render::CullMode::None;
        call.state.frontFace = render::FrontFace::CounterClockwise;
        const bool grass = options.grass && tex && tex->grass;
        if (grass)
            call.constants.flags |= render::DrawFlag::Grass;
        device.draw(call);
        ++calls;
        // OpenMM2 grass: the shell layers, blended from the roots up without
        // writing depth (CityRenderer::drawGrass).
        if (grass && options.grassLayers > 0 && !translucent && options.depthWrite) {
            call.constants.flags = (flags & ~render::DrawFlag::PixelLighting) | render::DrawFlag::Grass |
                                   render::DrawFlag::GrassShell;
            call.state.blend = render::BlendMode::Alpha;
            call.state.depthWrite = false;
            for (int layer = 1; layer <= options.grassLayers; ++layer) {
                call.constants.specular = static_cast<float>(layer) / static_cast<float>(options.grassLayers);
                device.draw(call);
                ++calls;
            }
        }
    }
    return calls;
}

ObjectDetail ObjectDetail::forLevel(int level) {
    switch (std::clamp(level, 0, 3)) {
    case 0: return {20.0f, 70.0f, 150.0f, 200.0f};
    case 1: return {30.0f, 90.0f, 175.0f, 250.0f};
    case 2: return {40.0f, 100.0f, 200.0f, 300.0f};
    default: return {70.0f, 130.0f, 200.0f, 300.0f};
    }
}

std::optional<asset::Lod> objectLod(float depth, float radius, const ObjectDetail& detail,
                                    std::optional<float> limit) {
    if (limit && depth > *limit)
        return std::nullopt;
    const float d = depth - radius;
    if (d > detail.vlow)
        return asset::Lod::VeryLow;
    if (d > detail.low)
        return asset::Lod::Low;
    if (d > detail.med)
        return asset::Lod::Medium;
    return asset::Lod::High;
}

} // namespace mm2::game
