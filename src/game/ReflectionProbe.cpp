#include "game/ReflectionProbe.h"

#include <algorithm>
#include <cmath>

namespace mm2::game {

Mat34 probeFaceCamera(const Vec3& center, std::size_t face) {
    const ProbeFace& f = kProbeFaces[face];
    const Vec3 back = f.forward * -1.0f;
    Mat34 m;
    m.m0 = f.up.cross(back); // right
    m.m1 = f.up;
    m.m2 = back;
    m.m3 = center;
    return m;
}

Mat44 probeProjection(float nearPlane, float farPlane) {
    return Mat44::perspective(1.5707964f, 1.0f, nearPlane, farPlane, true);
}

ProbeCell probeCell(std::size_t face, std::uint32_t size) {
    const ProbeFace& f = kProbeFaces[face];
    return {f.column * size, f.row * size, size};
}

ProbeLookup probeLookup(const Vec3& d, std::uint32_t size) {
    // The major axis picks the face (ties go to the first, as in mesh.frag).
    const float ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
    std::size_t face;
    if (ax >= ay && ax >= az)
        face = d.x >= 0.0f ? 0 : 1;
    else if (ay >= az)
        face = d.y >= 0.0f ? 2 : 3;
    else
        face = d.z >= 0.0f ? 4 : 5;
    const ProbeFace& f = kProbeFaces[face];
    const Vec3 right = f.up.cross(f.forward * -1.0f);
    const float z = d.dot(f.forward);
    const float edge = 1.0f - 3.0f / static_cast<float>(size);
    const float x = std::clamp(d.dot(right) / z, -edge, edge);
    const float y = std::clamp(d.dot(f.up) / z, -edge, edge);
    ProbeLookup out;
    out.face = face;
    out.u = (static_cast<float>(f.column) + x * 0.5f + 0.5f) / 3.0f;
    out.v = (static_cast<float>(f.row) + 0.5f - y * 0.5f) / 2.0f;
    return out;
}

} // namespace mm2::game
