#include "game/fx/ParticleRenderer.h"

#include <cmath>

namespace mm2::game::fx {
namespace {

// asMeshCardInfo::sQuad: x, y, u, v.
constexpr float kQuad[4][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {1, 1, 1, 1}, {-1, 1, 0, 1}};

std::uint32_t argbToRgba(std::uint32_t argb) {
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
}

// asMeshCardInfo::DrawShadows halves the colour and, as the original does,
// swaps red and blue on the way (0xAARRGGBB -> 0xAA, B/2, G/2, R/2).
std::uint32_t shadowColor(std::uint32_t c) {
    return ((((c >> 16) & 0xFEu) | (c & 0xFE00u)) >> 1) | ((c & 0xFEu) << 15) | (c & 0xFF000000u);
}

render::Vertex3D cardVertex(const Vec3& w, const Vec3& normal, std::uint32_t argb, float u, float v) {
    render::Vertex3D out{};
    out.position[0] = w.x;
    out.position[1] = w.y;
    out.position[2] = w.z;
    out.normal[0] = normal.x;
    out.normal[1] = normal.y;
    out.normal[2] = normal.z;
    out.color = argbToRgba(argb);
    out.uv0[0] = out.uv1[0] = u;
    out.uv0[1] = out.uv1[1] = v;
    return out;
}

} // namespace

ParticleRenderer::ParticleRenderer() {
    // asMeshCardInfo::Init: angle = r * 2pi / count, x' = c x - s y, y' = c y + s x.
    const float step = 6.2831855f / static_cast<float>(kRotations);
    for (int r = 0; r < kRotations; ++r) {
        const float a = static_cast<float>(r) * step;
        const float c = std::cos(a), s = std::sin(a);
        for (int i = 0; i < 4; ++i)
            m_rot[static_cast<std::size_t>(r)][static_cast<std::size_t>(i)] = {c * kQuad[i][0] - s * kQuad[i][1],
                                                                               c * kQuad[i][1] + s * kQuad[i][0]};
    }
}

void ParticleRenderer::begin() {
    m_vertices.clear();
    m_shadows.clear();
}

void ParticleRenderer::add(const SparkPos& p, int fw, int fh, const Mat34& cam) {
    if (m_vertices.size() + m_shadows.size() + 8 > 65535)
        return;
    // asMeshCardInfo::Draw: the rotation picks one of the 32 precomputed
    // quads, the frame one cell of the sheet (row-major).
    const auto& rot = m_rot[static_cast<std::size_t>(p.rotation & (kRotations - 1))];
    const int frame = std::max(0, static_cast<int>(p.frame));
    const float col = static_cast<float>(frame % fw), row = static_cast<float>((frame / fw) % fh);
    auto u = [&](int i) { return (kQuad[i][2] + col) / static_cast<float>(fw); };
    auto v = [&](int i) { return (kQuad[i][3] + row) / static_cast<float>(fh); };
    // asMeshCardInfo::DrawShadows: a flat card on the particle's height plane
    // while it is more than 1 cm above it.
    if ((p.flags & BirthRule::kShadow) && p.position.y - p.height > 0.01f) {
        const std::uint32_t color = shadowColor(p.color);
        for (int i = 0; i < 4; ++i) {
            const Vec2 o = rot[static_cast<std::size_t>(i)] * p.radius;
            m_shadows.push_back(
                cardVertex({o.x + p.position.x, p.height, o.y + p.position.z}, {0, 1, 0}, color, u(i), v(i)));
        }
    }
    for (int i = 0; i < 4; ++i) {
        const Vec2 o = rot[static_cast<std::size_t>(i)] * p.radius;
        const Vec3 w = cam.m0 * o.x + cam.m1 * o.y + p.position;
        m_vertices.push_back(cardVertex(w, cam.m2, p.color, u(i), v(i)));
    }
}

void ParticleRenderer::flush(render::Device& device, const WorldTexture* texture, const CardStyle& style) {
    if (m_vertices.empty() && m_shadows.empty())
        return;
    // Shadows go first so the cards blend over them.
    std::vector<render::Vertex3D> vertices = std::move(m_shadows);
    vertices.insert(vertices.end(), m_vertices.begin(), m_vertices.end());
    std::vector<std::uint16_t> indices;
    indices.reserve(vertices.size() / 4 * 6);
    for (std::size_t base = 0; base + 3 < vertices.size(); base += 4)
        for (std::uint16_t k : {0, 1, 2, 0, 2, 3})
            indices.push_back(static_cast<std::uint16_t>(base + k));

    render::DrawCall call;
    call.vertices = device.uploadTransient(render::BufferKind::Vertex, std::span<const render::Vertex3D>(vertices));
    call.indices = device.uploadTransient(render::BufferKind::Index, std::span<const std::uint16_t>(indices));
    call.indexType = render::IndexType::U16;
    call.count = static_cast<std::uint32_t>(indices.size());
    call.constants.world = Mat44::identity();
    call.constants.color = style.tint;
    // OpenMM2: no shadow from smoke, dust or sparks (render::DrawFlag::NoCast).
    call.constants.flags = render::DrawFlag::VertexColor | render::DrawFlag::AlphaTest | render::DrawFlag::NoCast;
    call.constants.alphaRef = 1.0f / 255.0f;
    if (style.fog)
        call.constants.flags |= render::DrawFlag::Fog;
    if (texture) {
        call.constants.flags |= render::DrawFlag::Texture0;
        render::SamplerDesc sampler = texture->sampler;
        sampler.addressU = sampler.addressV = render::AddressMode::Clamp;
        call.textures[0] = {texture->handle, sampler};
    }
    call.state.blend = style.blend;
    call.state.depthWrite = false;
    call.state.cull = render::CullMode::None;
    device.draw(call);
    m_cardsDrawn += static_cast<int>(indices.size() / 6);
    begin();
}

void ParticleRenderer::draw(render::Device& device, const Mat34& cameraBasis, const ParticleSystem& system,
                            const WorldTexture* texture, const CardStyle& style, float behind) {
    begin();
    const auto positions = system.positions();
    const auto info = system.info();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (behind > 0.0f) {
            SparkPos p = positions[i];
            p.position = p.position - info[i].velocity * behind;
            add(p, system.framesWide(), system.framesHigh(), cameraBasis);
        } else {
            add(positions[i], system.framesWide(), system.framesHigh(), cameraBasis);
        }
    }
    flush(device, texture, style);
}

} // namespace mm2::game::fx
