#include "city/Mm1Rail.h"

#include <algorithm>
#include <cmath>

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

int mm1SolveTurnType(const Mm1AiPath& cur, const Mm1AiPath& next) {
    const int n = cur.vertexCount;
    if (n < 2 || next.center.size() < 3 || static_cast<int>(cur.xAxis.size()) < n ||
        static_cast<int>(cur.zAxis.size()) < n)
        return 3;
    const Vec3 v = next.center[2] - next.center[1];
    const Vec3& x = cur.xAxis[static_cast<std::size_t>(n - 2)];
    const Vec3& z = cur.zAxis[static_cast<std::size_t>(n - 2)];
    const float lateral = (-x.x) * v.x + (-x.y) * v.y + (-x.z) * v.z;
    const float ahead = (-z.x) * v.x + (-z.y) * v.y + (-z.z) * v.z;
    const float a = std::atan2(lateral, ahead);
    if (a > 0.5f)
        return 0;
    return a < -0.5f ? 1 : 3;
}

Vec3 mm1CalcTurnIntersection(const Mm1AiPath& cur, const Mm1AiPath& next, int dir, float turnCenterPtOffset) {
    const auto n = static_cast<std::size_t>(cur.vertexCount);
    const std::size_t i = n - 2;
    if (n < 2 || cur.zAxis.size() < n || next.zAxis.size() < 2)
        return {};
    Vec3 p1, d1, p2, d2;
    float y = 0.0f;
    d1 = {-cur.zAxis[i].x, -cur.zAxis[i].y, -cur.zAxis[i].z};
    if (dir > 0) {
        if (cur.curb.size() < n || cur.xAxis.size() < n || next.curb.size() < 2)
            return {};
        const Vec3& x = cur.xAxis[i];
        const Vec3 shift{x.x * turnCenterPtOffset, x.y * turnCenterPtOffset, x.z * turnCenterPtOffset};
        p1 = {cur.curb[i].x + shift.x, cur.curb[i].y + shift.y, cur.curb[i].z + shift.z};
        p2 = next.curb[1];
        d2 = next.zAxis[1];
        y = p2.y;
    } else {
        if (cur.leftCurb.size() < n || next.leftCurb.size() < 2)
            return {};
        p1 = cur.leftCurb[i];
        const Vec3& z = cur.zAxis[i];
        const Vec3& l = next.leftCurb[1];
        p2 = {l.x - z.x * turnCenterPtOffset, l.y - z.y * turnCenterPtOffset, l.z - z.z * turnCenterPtOffset};
        d2 = {-next.zAxis[1].x, -next.zAxis[1].y, -next.zAxis[1].z};
        y = l.y;
    }
    // k = (d1.x (p2.z - p1.z) - d1.z (p2.x - p1.x)) / (d2.x d1.z - d2.z d1.x), in the asm's order.
    const float num = (p2.z * d1.x - d1.x * p1.z) - (p2.x * d1.z) + (d1.z * p1.x);
    const float den = d2.x * d1.z - d2.z * d1.x;
    const float k = num / den;
    return {d2.x * k + p2.x, y, d2.z * k + p2.z};
}

} // namespace mm2::city
