#include "city/Mm1Rail.h"

#include <algorithm>

namespace mm2::city {

float mm1CenterLength(const Mm1AiPath& path, int from, int to) {
    const auto n = static_cast<int>(path.centerOffsets.size());
    if (from < 0 || to < 0 || from >= n || to >= n)
        return 0.0f;
    return path.centerOffsets[static_cast<std::size_t>(to)] - path.centerOffsets[static_cast<std::size_t>(from)];
}

int mm1CenterIndex(const Mm1AiPath& path, float dist) {
    const int n = path.vertexCount;
    if (n <= 2 || static_cast<int>(path.centerOffsets.size()) < n)
        return n - 2;
    const float length = mm1CenterLength(path, 1, n - 2);
    // fcomp 0: not above 0 -> 0; else the length at most.
    if (dist <= 0.0f)
        dist = 0.0f;
    else if (!(dist < length))
        dist = length;
    const float base = path.centerOffsets[1];
    for (int i = 2; i < n; ++i)
        if (dist <= (path.centerOffsets[static_cast<std::size_t>(i)] - base) + 9.983003e-06f)
            return i;
    return n - 2;
}

Vec3 mm1SubSectionDir(const Mm1AiPath& path, int vertex, float scale) {
    if (vertex < 0 || vertex >= static_cast<int>(path.subSectionDirs.size()))
        return {};
    const Vec3& d = path.subSectionDirs[static_cast<std::size_t>(vertex)];
    return {scale * d.x, scale * d.y, scale * d.z};
}

Vec3 mm1IntersectionEntryVector(const Mm1AiPath& path, float scale) {
    const int i = path.vertexCount - 2;
    if (i < 0 || i >= static_cast<int>(path.zAxis.size()))
        return {};
    const Vec3& z = path.zAxis[static_cast<std::size_t>(i)];
    return {scale * -z.x, scale * -z.y, scale * -z.z};
}

Vec3 mm1IntersectionExitVector(const Mm1AiPath& path, float scale) {
    if (path.zAxis.size() < 3)
        return {};
    const Vec3& z = path.zAxis[2];
    return {scale * -z.x, scale * -z.y, scale * -z.z};
}

} // namespace mm2::city
