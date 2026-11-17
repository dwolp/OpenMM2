#pragma once

#include "game/fx/EffectLibrary.h"
#include "game/fx/ParticleRenderer.h"
#include "game/fx/Particles.h"

namespace mm2::game::fx {

// Rain around the camera, as MM2 sets it up for weather 3: 200 particles of
// tune/rain.asbirthrule spewed by the rule (200 per second) from texture
// ptx_rain (4 x 4 frames, a random one each), born 10 m above and 10 m in
// front of the camera (camera space (0, 10, -10), set every frame in
// cityLevel::DrawRooms). Drops stop and vanish below the rule's Height
// (0: the rain file has none). The caller skips drawing where the camera is
// under cover (see RaceScreen).
//
// MM2 has no snow weather (its menus offer weather 0-3); OpenMM2's snow
// option uses MM1's leftover tune/snow.asbirthrule (frames 5-7 of
// ptx_rain, the small flakes) from an emitter 6 m above and 6 m ahead of
// the camera on the horizontal (OpenMM2 addition).
class Weather {
public:
    enum class Kind { None, Rain, Snow };

    // Particles in the pool (the weather Init's 200).
    static constexpr int kParticles = 200;
    static constexpr float kRainScale = 2.5f; // OpenMM2: the particles are drawn this many times MM2's size
    static constexpr float kSnowScale = 4.0f;

    Weather(const EffectLibrary& library, Kind kind);
    Kind kind() const { return m_kind; }

    // Moves the emitter with the camera and runs the 60 Hz updates.
    void update(float dt, const Mat34& camera);
    void draw(render::Device& device, TextureLibrary& textures, ParticleRenderer& cards, const Mat34& camera);
    const ParticleSystem& particles() const { return m_system; }

private:
    Kind m_kind;
    BirthRule m_rule;
    ParticleSystem m_system;
    FixedTicker m_ticker;
    bool m_ok = false;
};

} // namespace mm2::game::fx
