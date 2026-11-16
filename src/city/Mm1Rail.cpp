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

float mm1TurnAngle(const Mm1AiPath& cur, const Mm1AiPath& next) {
    const auto n = static_cast<std::size_t>(cur.vertexCount);
    if (n < 3 || cur.xAxis.size() < n || cur.zAxis.size() < n || next.center.size() < 3)
        return 0.0f;
    const Vec3 v = next.center[2] - next.center[1];
    const Vec3& x = cur.xAxis[n - 2];
    const Vec3& z = cur.zAxis[n - 2];
    return std::atan2((-x.x) * v.x + (-x.y) * v.y + (-x.z) * v.z, (-z.x) * v.x + (-z.y) * v.y + (-z.z) * v.z);
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

float mm1RightEdge(const Mm1AiPath& path) {
    const int i = 2 * path.numLanes - 1;
    return i >= 0 && i < 10 ? path.laneLengths[static_cast<std::size_t>(i)] : 0.0f;
}

namespace {

// What CalcRailLength and CalcRailState both work out: A, and either the
// straight junction's gap or the turn's d1, d2 and setback q.
struct RailGeometry {
    bool valid = false;
    bool straight = true;
    float a0 = 0.0f;  // A: CenterLength(1, n-2)
    float gap = 0.0f; // straight: dist(next.center[1], cur.center[n-2])
    float d1 = 0.0f, d2 = 0.0f, q = 0.0f;
};

RailGeometry railGeometry(const Mm1AiPath& cur, const Mm1AiPath& next, const Mm1RailParams& rail) {
    RailGeometry g;
    const int n = cur.vertexCount;
    g.a0 = mm1CenterLength(cur, 1, n - 2);
    if (n < 2 || cur.center.size() < static_cast<std::size_t>(n) || cur.xAxis.size() < static_cast<std::size_t>(n) ||
        cur.zAxis.size() < static_cast<std::size_t>(n) || next.center.size() < 3 || next.zAxis.size() < 2)
        return g;
    g.valid = true;
    const std::size_t i = static_cast<std::size_t>(n - 2);
    const Vec3 v1 = next.center[2] - next.center[1];
    const Vec3& x = cur.xAxis[i];
    const Vec3& z = cur.zAxis[i];
    const float lateral = (-x.x) * v1.x + (-x.y) * v1.y + (-x.z) * v1.z;
    const float ahead = (-z.x) * v1.x + (-z.y) * v1.y + (-z.z) * v1.z;
    const float a = std::atan2(lateral, ahead);
    if (a > -0.7f && a < 0.7f) {
        g.gap = next.center[1].dist(cur.center[i]);
        return g;
    }
    g.straight = false;
    const float s = a < 0.0f ? -1.0f : 1.0f;
    const float availC = mm1RightEdge(cur) - rail.rSide;
    const float limit = 2.0f * mm1RightEdge(next) - (rail.lSide + rail.rSide);
    const float r0 = availC <= 0.0f ? 0.0f : (availC < limit ? availC : limit);
    const float theta = (3.14f - std::abs(a)) * 0.5f;
    const float radius = r0 / (1.0f - std::sin(theta));
    g.q = radius * std::cos(theta);
    const Vec3 p = mm1CalcTurnIntersection(cur, next, static_cast<int>(s), rail.turnCenterPtOffset);
    const Vec3 vc = p - cur.center[i];
    g.d1 = ((-z.x) * vc.x + (-z.y) * vc.y) + (-z.z) * vc.z;
    const Vec3 vn = p - next.center[1];
    const Vec3& zn = next.zAxis[1];
    g.d2 = (vn.x * zn.x + vn.y * zn.y) + vn.z * zn.z;
    return g;
}

} // namespace

float mm1CalcRailLength(const Mm1AiPath& cur, const Mm1AiPath* next, const Mm1RailParams& rail) {
    if (!next)
        return mm1CenterLength(cur, 1, cur.vertexCount - 2);
    const RailGeometry g = railGeometry(cur, *next, rail);
    if (!g.valid)
        return g.a0;
    if (g.straight)
        return g.gap + g.a0;
    return (((g.d1 + g.a0) - g.q) + (g.q + g.q)) - (g.q - g.d2);
}

int mm1CalcRailState(const Mm1AiPath& cur, const Mm1AiPath* next, const Mm1RailParams& rail, float dist) {
    if (!next)
        return 1;
    const RailGeometry g = railGeometry(cur, *next, rail);
    if (!g.valid)
        return 1;
    if (g.straight) {
        if (dist < g.a0)
            return 1;
        return (dist - g.a0) < g.gap ? 2 : 1;
    }
    const float start = (g.d1 + g.a0) - g.q; // the arc's start along the rail
    if (dist < start)
        return 1;
    const float into = dist - start;
    if (into < g.q + g.q)
        return 2;
    // After the arc the asm re-tests (q + (into - 2q)) < q, which only holds below the arc's end.
    return (g.q + (into - (g.q + g.q))) < g.q ? 2 : 1;
}

namespace {

// CalcXZPosition: the Hermite, t clamped at 0 from below, y = 0.
Vec3 hermiteXZ(const Vec3& p0, const Vec3& p1, const Vec3& t0, const Vec3& t1, float t) {
    Vec3 r = mm1RailCurve(p0, p1, t0, t1, t < 0.0f ? 0.0f : t);
    r.y = 0.0f;
    return r;
}

Vec3 offsetPoint(const Mm1AiPath& path, int vertex, int xVertex, float k) {
    const Vec3& c = path.center[static_cast<std::size_t>(vertex)];
    const Vec3& x = path.xAxis[static_cast<std::size_t>(xVertex)];
    return {c.x + x.x * k, c.y + x.y * k, c.z + x.z * k};
}

// The Hermite along a road at `dist` (the asm's on-road block, in both the
// current and the next road), the points moved along xAxis by k: segment
// idx = CenterIndex(dist); the first segment's start tangent is the exit
// vector, the last one's end tangent the entry vector, else the stored
// directions; the last segment's points use xAxis[idx] for both ends.
Vec3 roadHermite(const Mm1AiPath& path, float dist, float k) {
    const int n = path.vertexCount;
    const int idx = mm1CenterIndex(path, dist);
    const float len = mm1CenterLength(path, idx - 1, idx);
    Vec3 pa, pb, t0, t1;
    if (idx <= n - 3) {
        if (idx == 2) {
            pa = offsetPoint(path, 1, 1, k);
            pb = offsetPoint(path, 2, 2, k);
            t0 = mm1IntersectionExitVector(path, len);
            t1 = mm1SubSectionDir(path, 2, len);
        } else {
            pa = offsetPoint(path, idx - 1, idx - 1, k);
            pb = offsetPoint(path, idx, idx, k);
            t0 = mm1SubSectionDir(path, idx - 1, len);
            t1 = mm1SubSectionDir(path, idx, len);
        }
    } else {
        pa = offsetPoint(path, idx - 1, idx, k);
        pb = offsetPoint(path, idx, idx, k);
        t0 = mm1SubSectionDir(path, idx - 1, len);
        t1 = mm1IntersectionEntryVector(path, len);
    }
    return hermiteXZ(pa, pb, t0, t1, (dist - mm1CenterLength(path, 1, idx - 1)) / len);
}

// The turn case (|a| >= 0.7), see the header.
Mm1RailPosition turnPosition(const Mm1AiPath& cur, const Mm1AiPath& next, const Mm1RailParams& rail, const Vec3& carPos,
                             float dist, float offset, float a, float a0) {
    Mm1RailPosition out;
    const int n = cur.vertexCount;
    const std::size_t i = static_cast<std::size_t>(n - 2);
    if (cur.zAxis.size() < static_cast<std::size_t>(n) || next.zAxis.size() < 2 || next.xAxis.size() < 2)
        return out;
    const float s = a < 0.0f ? -1.0f : 1.0f;
    const float absA = std::abs(a);
    const float theta = (3.14f - absA) * 0.5f;
    float rpre = mm1RightEdge(cur) - s * offset;
    if (s == 1.0f)
        rpre -= rail.turnCenterPtOffset;
    const float limit = 2.0f * mm1RightEdge(next) - (rail.rSide + rail.lSide);
    const float r0 = rpre <= 0.0f ? 0.0f : (rpre < limit ? rpre : limit);
    const float rarc = r0 / (1.0f - std::sin(theta));
    const float q = std::cos(theta) * rarc;
    const Vec3 p = mm1CalcTurnIntersection(cur, next, static_cast<int>(s), rail.turnCenterPtOffset);
    const Vec3& z = cur.zAxis[i];
    const Vec3 vc = p - cur.center[i];
    const float d1 = ((-z.x) * vc.x + (-z.y) * vc.y) + (-z.z) * vc.z;
    const float start = (d1 + a0) - q;
    if (dist < start) {
        out.pos = roadHermite(cur, dist, -offset);
        out.state = (d1 + dist) < q ? 2 : 1;
        return out;
    }
    const float into = dist - start;
    const float twoQ = q + q;
    if (into < twoQ) {
        const Vec3& x = cur.xAxis[i];
        const float w = (rarc - r0) * s;
        const float phi = ((into / twoQ) * s) * a;
        // C0 = P + q Z - w X; pos = C0 + (s Rarc cos phi) X - (Rarc sin phi) Z.
        const float c1 = (std::cos(phi) * rarc) * s;
        const float c2 = std::sin(phi) * rarc;
        out.pos = {(p.x + q * z.x - w * x.x) + c1 * x.x - c2 * z.x, (p.y + q * z.y - w * x.y) + c1 * x.y - c2 * z.y,
                   (p.z + q * z.z - w * x.z) + c1 * x.z - c2 * z.z};
        out.state = 2;
        return out;
    }
    const Vec3 vn = p - next.center[1];
    const Vec3& zn = next.zAxis[1];
    const float d2 = (vn.x * zn.x + vn.y * zn.y) + vn.z * zn.z;
    const float dn = (into - twoQ) + (q - d2);
    // The car's own place across the next road (x, z only), kept within the road less r0.
    const Vec3 u = carPos - next.center[1];
    const Vec3& xn = next.xAxis[1];
    const float lat = -(xn.x * u.x + xn.z * u.z);
    const float lim = mm1RightEdge(next) - r0;
    const float o2 = lat < -lim ? -lim : (lat > lim ? lim : lat);
    const int nn = next.vertexCount;
    const int idx = mm1CenterIndex(next, dn);
    const float len = mm1CenterLength(next, idx - 1, idx);
    Vec3 pa, pb, t0, t1;
    if (idx == 2) {
        pa = offsetPoint(next, 1, 1, -o2);
        pb = offsetPoint(next, 2, 2, -o2);
        t0 = mm1IntersectionExitVector(next, len);
        t1 = mm1SubSectionDir(next, 2, len);
        out.pos = hermiteXZ(pa, pb, t0, t1, dn / len);
    } else {
        pa = offsetPoint(next, idx - 1, idx - 1, -o2);
        pb = offsetPoint(next, idx, idx, -o2);
        t0 = mm1SubSectionDir(next, idx - 1, len);
        t1 = mm1SubSectionDir(next, idx, len);
        out.pos = hermiteXZ(pa, pb, t0, t1, (dn - mm1CenterLength(next, 1, idx - 1)) / len);
    }
    (void)nn;
    out.state = (d1 + dn) < q ? 2 : 1;
    return out;
}

} // namespace

Mm1RailPosition mm1CalcCopRailPosition(const Mm1AiPath& cur, const Mm1AiPath* next, const Mm1RailParams& rail,
                                       const Vec3& carPos, float dist, float offset) {
    Mm1RailPosition out;
    const int n = cur.vertexCount;
    if (n < 4 || cur.center.size() < static_cast<std::size_t>(n) || cur.xAxis.size() < static_cast<std::size_t>(n))
        return out;
    const float a0 = mm1CenterLength(cur, 1, n - 2);
    const float neg = -offset;
    if (!next) {
        out.state = 1;
        if (dist < a0) {
            out.pos = roadHermite(cur, dist, neg);
            return out;
        }
        // Past the end: the last segment, extended.
        const Vec3 a = offsetPoint(cur, n - 3, n - 3, neg);
        const Vec3 b = offsetPoint(cur, n - 2, n - 2, neg);
        const float len = mm1CenterLength(cur, n - 3, n - 2);
        const float t = ((len + dist) - a0) / len;
        out.pos = {(b.x - a.x) * t + a.x, (b.y - a.y) * t + a.y, (b.z - a.z) * t + a.z};
        return out;
    }
    // With a next road: straight or a turn?
    if (next->center.size() < 3 || next->xAxis.size() < static_cast<std::size_t>(next->vertexCount) ||
        cur.zAxis.size() < static_cast<std::size_t>(n))
        return out;
    const std::size_t i = static_cast<std::size_t>(n - 2);
    const Vec3 v1 = next->center[2] - next->center[1];
    const Vec3& cx = cur.xAxis[i];
    const Vec3& cz = cur.zAxis[i];
    const float lateral = (-cx.x) * v1.x + (-cx.y) * v1.y + (-cx.z) * v1.z;
    const float ahead = (-cz.x) * v1.x + (-cz.y) * v1.y + (-cz.z) * v1.z;
    const float a = std::atan2(lateral, ahead);
    if (!(a > -0.7f && a < 0.7f))
        return turnPosition(cur, *next, rail, carPos, dist, offset, a, a0);
    if (dist < a0) {
        out.pos = roadHermite(cur, dist, neg);
        out.state = dist < 10.0f ? 2 : 1;
        return out;
    }
    // The junction: the next road's offset o' (rail +0xC is the car's right extent).
    const float wN = mm1RightEdge(*next);
    float m = wN - rail.rSide - 3.0f;
    if (m < 0.0f)
        m = 0.0f;
    float v = next->divided ? rail.rSide - (-3.0f) : 0.0f;
    if (!(v <= m))
        v = m;
    float o2;
    if (!(offset < 0.0f))
        o2 = offset <= v ? v : (offset < m ? offset : m);
    else
        o2 = offset <= -m ? -m : (offset < -v ? offset : -v);
    const Vec3 entry = offsetPoint(cur, n - 2, n - 2, -offset);
    const Vec3 exit = offsetPoint(*next, 1, 1, -o2);
    const float gap = entry.dist(exit);
    const float into = dist - a0;
    if (into < gap) {
        out.pos = hermiteXZ(entry, exit, mm1IntersectionEntryVector(cur, gap), mm1IntersectionExitVector(*next, gap),
                            into / gap);
        out.state = 2;
        return out;
    }
    out.state = 1;
    const float d2 = into - gap;
    const int nn = next->vertexCount;
    const int idx = mm1CenterIndex(*next, d2);
    if (idx < nn - 2) {
        out.pos = roadHermite(*next, d2, -o2);
        return out;
    }
    // Beyond the next road: a fixed point (L + 10) / L along its last segment.
    const Vec3 pa = offsetPoint(*next, nn - 3, nn - 3, -o2);
    const Vec3 pb = offsetPoint(*next, nn - 2, nn - 2, -o2);
    const float len = mm1CenterLength(*next, nn - 3, nn - 2);
    const float t = (len - (-10.0f)) / len;
    out.pos = {(pb.x - pa.x) * t + pa.x, (pb.y - pa.y) * t + pa.y, (pb.z - pa.z) * t + pa.z};
    return out;
}

Mm1AiPath mm1PathFromVerts(std::span<const Vec3> verts, int id) {
    Mm1AiPath p;
    const auto n = verts.size();
    p.id = id;
    p.vertexCount = static_cast<int>(n);
    p.numLanes = 1;
    p.numSidewalks = 1;
    p.speedLimit = 15.0f;
    p.length = 10.0f; // aiPath +0x28, set to 10 here (also LaneLengths[1])
    p.laneWidths = {7.0f, 7.0f, 0.0f, 0.0f, 0.0f};
    p.laneLengths = {0.0f, 10.0f, 0, 0, 0, 0, 0, 0, 0, 0};
    p.center.assign(verts.begin(), verts.end());
    p.normal.assign(n, Vec3{0.0f, 1.0f, 0.0f});
    p.zAxis.assign(n, Vec3{});
    p.xAxis.assign(n, Vec3{});
    p.subSectionDirs.assign(n, Vec3{});
    p.edge.assign(n, Vec3{});
    p.curb.assign(n, Vec3{});
    p.leftCurb.assign(n, Vec3{});
    p.centerOffsets.assign(n, 0.0f);
    p.subSectionOffsets.assign(n, 0.0f);
    if (n < 3)
        return p;
    auto unit = [](Vec3 v) {
        const float inv = 1.0f / std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return Vec3{v.x * inv, v.y * inv, v.z * inv};
    };
    for (std::size_t i = 2; i + 1 < n; ++i)
        p.zAxis[i] = unit(verts[i - 1] - verts[i]);
    p.zAxis[1] = p.zAxis[2];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const Vec3& z = p.zAxis[i];
        const Vec3& nm = p.normal[i];
        p.xAxis[i] = {nm.z * z.y - z.z * nm.y, nm.x * z.z - z.x * nm.z, z.x * nm.y - nm.x * z.y};
    }
    p.xAxis[n - 1] = p.xAxis[n - 2];
    for (std::size_t i = 2; i + 1 < n; ++i)
        p.subSectionDirs[i] = unit(verts[i + 1] - verts[i - 1]);
    p.edge[0] = p.curb[0] = p.leftCurb[0] = verts[0];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const Vec3& x = p.xAxis[i];
        const Vec3& c = verts[i];
        p.edge[i] = p.curb[i] = {c.x - x.x * 10.0f, c.y - x.y * 10.0f, c.z - x.z * 10.0f};
        p.leftCurb[i] = {c.x + x.x * 10.0f, c.y + x.y * 10.0f, c.z + x.z * 10.0f};
    }
    p.edge[n - 1] = p.curb[n - 1] = p.leftCurb[n - 1] = verts[n - 1];
    float run = 0.0f;
    for (std::size_t i = 1; i < n; ++i) {
        run = verts[i].dist(verts[i - 1]) + run;
        p.centerOffsets[i] = p.subSectionOffsets[i] = run;
    }
    return p;
}

Mm1AiPath mm1StartLink(const Vec3& p0, const Vec3& p1) {
    const Vec3 d = p1 - p0;
    // The asm computes p0 - d * f with f = -0.2, -0.3, -0.7, -0.8.
    auto at = [&](float f) { return Vec3{p0.x - d.x * f, p0.y - d.y * f, p0.z - d.z * f}; };
    const Vec3 verts[6] = {p0, at(-0.2f), at(-0.3f), at(-0.7f), at(-0.8f), p1};
    return mm1PathFromVerts(verts, 0x2706);
}

Mm1RoadPos mm1CenterDistance(const Mm1AiPath& path, const Vec3& pos) {
    Mm1RoadPos r;
    const int n = path.vertexCount;
    if (n <= 2 || path.center.size() < static_cast<std::size_t>(n) || path.zAxis.size() < static_cast<std::size_t>(n) ||
        path.xAxis.size() < static_cast<std::size_t>(n))
        return r;
    for (int i = 2; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const Vec3 d = pos - path.center[ui];
        const Vec3& z = path.zAxis[ui];
        const float a = (z.x * d.x + z.y * d.y) + z.z * d.z;
        if (a < 0.0f)
            continue;
        float lateral;
        if (i == n - 1) {
            const auto v = static_cast<std::size_t>(n - 2);
            const float dx = pos.x - path.center[v].x, dz = pos.z - path.center[v].z;
            lateral = -(path.xAxis[v].x * dx + path.xAxis[v].z * dz);
        } else {
            lateral = -(d.x * path.xAxis[ui].x + d.z * path.xAxis[ui].z);
        }
        if ((path.length - -5.0f) > std::abs(lateral)) {
            r.found = true;
            r.vertex = i;
            r.roadDist = mm1CenterLength(path, 1, i) - a;
            r.distToSide = lateral;
            return r;
        }
    }
    return r;
}

int mm1DetermineOppRoadPosInfo(const Mm1AiMap& map, const Vec3& pos, const Vec3& carBack, int pathId,
                               Mm1RoadPos* out) {
    if (pathId < 0 || static_cast<std::size_t>(pathId) >= map.paths.size()) {
        *out = {};
        return pathId;
    }
    const Mm1AiPath& path = map.paths[static_cast<std::size_t>(pathId)];
    *out = mm1CenterDistance(path, pos);
    const auto v = static_cast<std::size_t>(out->vertex);
    if (v < path.zAxis.size()) {
        const Vec3& z = path.zAxis[v];
        if ((z.x * carBack.x + z.y * carBack.y) + z.z * carBack.z > 0.0f)
            return pathId;
    }
    const int oncoming = path.oncoming >= 0 && static_cast<std::size_t>(path.oncoming) < map.paths.size()
                             ? path.oncoming
                             : pathId;
    const Mm1RoadPos first = *out;
    *out = mm1CenterDistance(map.paths[static_cast<std::size_t>(oncoming)], pos);
    if (!out->found) {
        // The asm overwrites only on success; keep the first query's numbers.
        out->roadDist = first.roadDist;
        out->distToSide = first.distToSide;
    }
    return oncoming;
}

int mm1PredictIntersectionPath(const Mm1AiMap& map, int intersectionId, const Vec3& carBack, bool reverse) {
    if (intersectionId < 0 || static_cast<std::size_t>(intersectionId) >= map.intersections.size())
        return -1;
    const Vec3 v = reverse ? carBack : Vec3{-carBack.x, -carBack.y, -carBack.z};
    float best = -1000000.0f; // -0x368BDC10 as the asm's float
    int bestPath = -1;
    for (const int id : map.intersections[static_cast<std::size_t>(intersectionId)].paths) {
        if (id < 0 || static_cast<std::size_t>(id) >= map.paths.size())
            continue;
        const Mm1AiPath& p = map.paths[static_cast<std::size_t>(id)];
        if (p.numSidewalks == 0 || p.center.size() < 3)
            continue;
        const Vec3 d = p.center[2] - p.center[1];
        const float score = (d.x * v.x + d.y * v.y) + d.z * v.z;
        if (score > best) {
            best = score;
            bestPath = id;
        }
    }
    return bestPath;
}

Mm1ArcProgress mm1IntersectionArcProgress(const Mm1AiPath& cur, const Mm1AiPath& next, const Mm1RailParams& rail,
                                          const Vec3& carPos, float distToSide, float speed) {
    Mm1ArcProgress out;
    const int n = cur.vertexCount;
    if (n < 3 || cur.center.size() < static_cast<std::size_t>(n) || cur.xAxis.size() < static_cast<std::size_t>(n) ||
        cur.zAxis.size() < static_cast<std::size_t>(n) || next.center.size() < 3 || next.zAxis.size() < 2)
        return out;
    const std::size_t i = static_cast<std::size_t>(n - 2);
    const Vec3 v1 = next.center[2] - next.center[1];
    const Vec3& x = cur.xAxis[i];
    const Vec3& z = cur.zAxis[i];
    const float a = std::atan2((-x.x) * v1.x + (-x.y) * v1.y + (-x.z) * v1.z,
                               (-z.x) * v1.x + (-z.y) * v1.y + (-z.z) * v1.z);
    if (a > -0.7f && a < 0.7f)
        return out;
    const float s = a < 0.0f ? -1.0f : 1.0f;
    const float rpre = (mm1RightEdge(cur) - distToSide * s) - rail.rSide;
    const float limit = 2.0f * mm1RightEdge(next) - rail.rSide;
    const float r0 = rpre <= 0.5f ? 0.5f : (rpre < limit ? rpre : limit);
    const float theta = (3.14f - std::abs(a)) * 0.5f;
    const float radius = r0 / (1.0f - std::sin(theta));
    const float q = std::cos(theta) * radius;
    const float a0 = mm1CenterLength(cur, 1, n - 2);
    const Vec3 p = mm1CalcTurnIntersection(cur, next, static_cast<int>(s), rail.turnCenterPtOffset);
    const Vec3 vc = p - cur.center[i];
    const float d1 = ((-z.x) * vc.x + (-z.y) * vc.y) + (-z.z) * vc.z;
    const float w = radius - r0;
    const Vec3 centre{p.x + z.x * q - x.x * (w * s), p.y + z.y * q - x.y * (w * s), p.z + z.z * q - x.z * (w * s)};
    const Vec3 rel = carPos - centre;
    const float lat = (x.x * s) * rel.x + (x.y * s) * rel.y + (x.z * s) * rel.z;
    const float ahead = (-z.x) * rel.x + (-z.y) * rel.y + (-z.z) * rel.z;
    const float phi = std::atan2(ahead, lat);
    const float rho = std::sqrt(lat * lat + ahead * ahead);
    out.valid = true;
    out.roadDist = ((d1 + a0) - q) + 2.0f * ((phi * q) / (s * a));
    float look = (rho - radius) + (0.488f * (speed * speed)) / (23.76f * radius);
    out.targetPtOffset = look <= 7.0f ? 7.0f : (look < 20.0f ? look : 20.0f);
    return out;
}

Mm1RoadPos mm1ProjectOffRoad(const Mm1AiMap& map, const Mm1AiPath& cur, const Vec3& pos, int pathId, float rSide) {
    Mm1RoadPos r;
    const int n = cur.vertexCount;
    if (n <= 2 || cur.center.size() < static_cast<std::size_t>(n) || cur.zAxis.size() < static_cast<std::size_t>(n) ||
        cur.xAxis.size() < static_cast<std::size_t>(n))
        return r;
    for (int i = 2; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const Vec3 d = pos - cur.center[ui];
        const Vec3& z = cur.zAxis[ui];
        const float dot = (z.x * d.x + z.y * d.y) + z.z * d.z;
        if (dot < 0.0f)
            continue;
        float lateral;
        if (i == n - 1) {
            const auto v = static_cast<std::size_t>(n - 2);
            const float dx = pos.x - cur.center[v].x, dz = pos.z - cur.center[v].z;
            lateral = -(cur.xAxis[v].x * dx + cur.xAxis[v].z * dz);
        } else {
            lateral = -(d.x * cur.xAxis[ui].x + d.z * cur.xAxis[ui].z);
        }
        float limit = 0.0f;
        if (pathId >= 0 && static_cast<std::size_t>(pathId) < map.paths.size())
            limit = mm1RightEdge(map.paths[static_cast<std::size_t>(pathId)]) - rSide;
        if (limit < 0.0f)
            limit = 0.0f;
        r.found = true;
        r.vertex = i;
        r.distToSide = lateral < -limit ? -limit : (lateral > limit ? limit : lateral);
        r.roadDist = mm1CenterLength(cur, 1, i) - dot;
        return r;
    }
    return r;
}

} // namespace mm2::city
