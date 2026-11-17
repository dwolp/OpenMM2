#include "game/fx/Weather.h"

namespace mm2::game::fx {

Weather::Weather(const EffectLibrary& library, Kind kind) : m_kind(kind) {
    const BirthRule* r = kind == Kind::Rain ? library.rule("rain") : kind == Kind::Snow ? library.rule("snow") : nullptr;
    if (!r)
        return;
    m_rule = *r;
    // OpenMM2: the flakes and drops are drawn at MM2's size, a few pixels at the distance they fall
    // round the car, so they barely show. Scaled up so the weather reads (not in MM2).
    const float scale = kind == Kind::Snow ? kSnowScale : kRainScale;
    m_rule.radius *= scale;
    m_rule.radiusVar *= scale;
    const ParticleSheet sheet = EffectLibrary::rainSheet();
    m_system.init(kParticles, sheet.framesWide, sheet.framesHigh);
    m_system.setBirthRule(&m_rule);
    m_ok = true;
}

void Weather::update(float dt, const Mat34& camera) {
    if (!m_ok)
        return;
    if (m_kind == Kind::Rain) {
        m_rule.position = camera.transform({0.0f, 10.0f, -10.0f});
    } else {
        Vec3 forward = -camera.m2;
        forward.y = 0.0f;
        forward = forward.mag2() > 1e-6f ? forward.normalized() : Vec3{0, 0, -1};
        m_rule.position = camera.m3 + forward * 6.0f + Vec3{0, 6.0f, 0};
    }
    for (int i = m_ticker.advance(dt); i > 0; --i)
        m_system.update(FixedTicker::kStep);
}

void Weather::draw(render::Device& device, TextureLibrary& textures, ParticleRenderer& cards, const Mat34& camera) {
    if (!m_ok || !m_system.count())
        return;
    // asParticles::SetTexture by name asks gfxGetTexture for no mipmaps.
    // OpenMM2: between the last two 60 Hz updates (FixedTicker::behind).
    cards.draw(device, camera, m_system, textures.get(EffectLibrary::rainSheet().texture, false), {},
               m_ticker.behind());
}

} // namespace mm2::game::fx
