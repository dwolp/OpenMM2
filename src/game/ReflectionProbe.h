#pragma once

// OpenMM2 live reflections (tweak LiveReflections, app/Tweaks.h; not in MM2).
//
// The world around the player's car drawn each frame into six 90-degree
// views, laid out in a 3 x 2 atlas (render::Device::beginProbePass): the
// car's paint and glass then reflect the real city, traffic and sky instead
// of MM2's fixed refl_dc sphere map. mesh.frag's probeColor looks a
// direction up with the same face bases as probeFace below; tests check the
// two agree.

#include "core/Math.h"

#include <array>
#include <cstdint>

namespace mm2::game {

// One face: the direction it looks along and its up, and its cell in the
// atlas (column 0..2, row 0..1, row 0 at the top as a viewport places it).
struct ProbeFace {
    Vec3 forward;
    Vec3 up;
    std::uint32_t column = 0, row = 0;
};

// +X, -X, +Y, -Y, +Z, -Z; the faces looking up and down take -Z and +Z as
// up. The cells run along the top row, then the bottom.
inline constexpr std::array<ProbeFace, 6> kProbeFaces{{
    {{1, 0, 0}, {0, 1, 0}, 0, 0},
    {{-1, 0, 0}, {0, 1, 0}, 1, 0},
    {{0, 1, 0}, {0, 0, -1}, 2, 0},
    {{0, -1, 0}, {0, 0, 1}, 0, 1},
    {{0, 0, 1}, {0, 1, 0}, 1, 1},
    {{0, 0, -1}, {0, 1, 0}, 2, 1},
}};

// The camera of face `face` at `center` (Angel convention: rows right, up,
// back, position; the camera looks down -Z).
Mat34 probeFaceCamera(const Vec3& center, std::size_t face);

// The 90-degree square projection every face draws with (0..1 depth).
Mat44 probeProjection(float nearPlane, float farPlane);

// The atlas pixel rectangle of face `face` for faces `size` texels square.
struct ProbeCell {
    std::uint32_t x = 0, y = 0, size = 0;
};
ProbeCell probeCell(std::size_t face, std::uint32_t size);

// mesh.frag's lookup: the face a direction falls in and where in the atlas
// (u right, v down from the top, 0..1), the face coordinates held a texel
// and a half inside the cell's edge so bilinear filtering never reads the
// neighbouring face.
struct ProbeLookup {
    std::size_t face = 0;
    float u = 0, v = 0;
};
ProbeLookup probeLookup(const Vec3& direction, std::uint32_t size);

} // namespace mm2::game
