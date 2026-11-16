#include "city/Mm1AiNav.h"

#include <algorithm>
#include <cmath>

namespace mm2::city {

Mm1AiNav::Mm1AiNav(const Mm1AiMap& map, std::function<int(const Vec3&)> findRoom)
    : m_map(map), m_findRoom(std::move(findRoom)) {
    for (const Mm1AiPath& p : map.paths) {
        const std::size_t n = p.center.size();
        if (n < 4)
            continue; // the asm needs n - 1 > 2
        for (std::size_t i = 2; i + 1 < n; ++i) {
            const Vec3& a = p.center[i - 1];
            const Vec3& b = p.center[i];
            // c[i-1] - (c[i] - c[i-1]) * -0.5
            const Vec3 mid{a.x - (b.x - a.x) * -0.5f, a.y - (b.y - a.y) * -0.5f, a.z - (b.z - a.z) * -0.5f};
            const int room = m_findRoom(mid);
            if (room != 0)
                m_rooms[room].push_back({p.id, 1});
        }
    }
    for (const Mm1AiIntersection& x : map.intersections) {
        const int room = m_findRoom(x.position);
        if (room == 0)
            continue;
        auto& list = m_rooms[room];
        const bool has = std::any_of(list.begin(), list.end(),
                                     [&](const Mm1Component& c) { return c.type == 2 && c.id == x.id; });
        if (!has)
            list.push_back({x.id, 2});
    }
}

const std::vector<Mm1Component>& Mm1AiNav::components(int room) const {
    static const std::vector<Mm1Component> none;
    const auto it = m_rooms.find(room);
    return it == m_rooms.end() ? none : it->second;
}

Mm1AiNav::Result Mm1AiNav::mapComponent(const Vec3& pos, int typeIn, int hint) const {
    Result r;
    r.type = typeIn;
    const int room = m_findRoom(pos);
    if (room == 0) {
        r.id = hint;
        if (hint == -1 && typeIn == 1)
            r.type = 0;
        return r;
    }
    r.room = room;
    const auto& list = components(room);
    for (const Mm1Component& c : list)
        if (c.id == hint) {
            r.id = c.id;
            r.type = c.type;
            return r;
        }
    for (const Mm1Component& c : list)
        if (c.type == 2) {
            r.id = c.id;
            r.type = 2;
            return r;
        }
    for (const Mm1Component& c : list) {
        if (c.id >= 0 && static_cast<std::size_t>(c.id) < m_map.paths.size() &&
            m_map.paths[static_cast<std::size_t>(c.id)].numSidewalks != 0) {
            r.id = c.id;
            r.type = c.type;
            return r;
        }
    }
    r.id = -1;
    r.type = 0;
    return r;
}

namespace {

float clampSide(float d, float limit, bool divided, float rSide) {
    if (limit < 0.0f)
        limit = 0.0f;
    if (!divided)
        return d < -limit ? -limit : (d > limit ? limit : d);
    const float lo = std::min(rSide + 3.0f, limit);
    return d <= lo ? lo : (d < limit ? d : limit);
}

const Mm1AiPath* pathAt(const Mm1AiMap& map, int id) {
    return id >= 0 && static_cast<std::size_t>(id) < map.paths.size() ? &map.paths[static_cast<std::size_t>(id)]
                                                                       : nullptr;
}

// The turn angle of `next` off `cur`'s last frame.
float turnAngle(const Mm1AiPath& cur, const Mm1AiPath& next) {
    const auto i = static_cast<std::size_t>(cur.vertexCount - 2);
    if (cur.vertexCount < 3 || cur.xAxis.size() <= i || cur.zAxis.size() <= i || next.center.size() < 3)
        return 0.0f;
    const Vec3 v = next.center[2] - next.center[1];
    const Vec3& x = cur.xAxis[i];
    const Vec3& z = cur.zAxis[i];
    return std::atan2((-x.x) * v.x + (-x.y) * v.y + (-x.z) * v.z, (-z.x) * v.x + (-z.y) * v.y + (-z.z) * v.z);
}

// CenterLength(cur, 1, n-2) + |p.c1 - cur.c[n-2]| (x and z) - Z1.(pos - c1), p's vertex 1.
float junctionRoadDist(const Mm1AiPath& cur, const Mm1AiPath& p, const Vec3& pos) {
    const Vec3& a = cur.center[static_cast<std::size_t>(cur.vertexCount - 2)];
    const Vec3& c1 = p.center[1];
    const Vec3& z1 = p.zAxis[1];
    const Vec3 d = pos - c1;
    return (mm1CenterLength(cur, 1, cur.vertexCount - 2) + (std::abs(c1.x - a.x) + std::abs(c1.z - a.z))) -
           ((z1.x * d.x + z1.y * d.y) + z1.z * d.z);
}

} // namespace

Mm1OppResult mm1DetermineOppMapComponent(const Mm1AiMap& map, const Mm1AiNav& nav, const Mm1OppRail& rail,
                                         const Mm1OppInput& in) {
    Mm1OppResult out;
    const Mm1AiNav::Result comp = nav.mapComponent(in.pos, in.type, in.pathId);
    out.type = comp.type;
    const Mm1AiPath* cur = pathAt(map, rail.cur);
    const Mm1AiPath* next = pathAt(map, rail.next);
    if (!cur)
        return out;
    const int n = cur->vertexCount;
    const float rSide = rail.params.rSide;
    out.id = cur->id; // the type-0 return
    if (n < 3 || cur->center.size() < static_cast<std::size_t>(n) || cur->zAxis.size() < static_cast<std::size_t>(n) ||
        cur->xAxis.size() < static_cast<std::size_t>(n))
        return out;

    if (comp.type == 2) { // an intersection
        out.id = comp.id;
        out.vertex = n - 2;
        if (!next) {
            const Mm1AiPath* pp = pathAt(map, mm1PredictIntersectionPath(map, comp.id, in.carBack, false));
            if (!pp || pp->center.size() < 2 || pp->zAxis.size() < 2)
                return out;
            out.roadDist = junctionRoadDist(*cur, *pp, in.pos);
            out.hasRoadDist = true;
            return out;
        }
        if (next->center.size() < 3 || next->zAxis.size() < 2)
            return out;
        const Mm1ArcProgress arc =
            mm1IntersectionArcProgress(*cur, *next, rail.params, in.pos, in.distToSide, in.speed);
        if (arc.valid) {
            out.roadDist = arc.roadDist;
            out.targetPtOffset = arc.targetPtOffset;
        } else {
            out.roadDist = junctionRoadDist(*cur, *next, in.pos);
        }
        out.hasRoadDist = true;
        return out;
    }

    if (comp.type == 1) { // a road
        Mm1RoadPos rp;
        out.id = mm1DetermineOppRoadPosInfo(map, in.pos, in.carBack, comp.id, &rp);
        out.vertex = rp.vertex;
        out.roadDist = rp.roadDist;
        out.hasRoadDist = true;
        float d = rp.distToSide;
        const bool arrived = in.lastType != 1; // the car has come onto a road: the rail follows it
        if (arrived)
            out.newCur = out.id;
        if (!next) {
            out.distToSide = d;
            out.hasDistToSide = true;
            return out;
        }
        if (next->center.size() < 3 || next->zAxis.size() < 2)
            return out;
        const float a = turnAngle(*cur, *next);
        const bool divided = cur->divided || next->divided;
        auto sideOut = [&](float limit) {
            if (cur->numSidewalks == 0 || in.roomIsEnclosed)
                out.distToSide = 0.0f;
            else
                out.distToSide = clampSide(d, limit, divided, rSide);
            out.hasDistToSide = true;
        };
        if (a > -0.7f && a < 0.7f) {
            out.targetPtOffset = 20.0f;
            sideOut(mm1RightEdge(*cur) - rSide - 3.0f);
            return out;
        }
        // A sharp turn at the road's end.
        const float s = a < 0.0f ? -1.0f : 1.0f;
        const float wC = std::max(mm1RightEdge(*cur) - rSide, 0.0f);
        const float x2 = 2.0f * mm1RightEdge(*next) - rSide;
        // The far-edge shift: a car too far out on the turn's outside moves 4 m in.
        if (s > 0.0f) {
            if (wC - d > x2)
                d -= -4.0f;
        } else if (wC + d > x2) {
            d -= 4.0f;
        }
        const float rpre = mm1RightEdge(*cur) - s * d;
        const float r0 = rpre <= 0.5f ? 0.5f : (rpre < x2 ? rpre : x2);
        const float theta = (3.14f - std::abs(a)) * 0.5f;
        const float radius = r0 / (1.0f - std::sin(theta));
        const float q = std::cos(theta) * radius;
        const float a0 = mm1CenterLength(*cur, 1, n - 2);
        const Vec3 p = mm1CalcTurnIntersection(*cur, *next, static_cast<int>(s), rail.params.turnCenterPtOffset);
        const Vec3& z = cur->zAxis[static_cast<std::size_t>(n - 2)];
        const Vec3& x = cur->xAxis[static_cast<std::size_t>(n - 2)];
        const Vec3 vc = p - cur->center[static_cast<std::size_t>(n - 2)];
        const float d1 = ((-z.x) * vc.x + (-z.y) * vc.y) + (-z.z) * vc.z;
        const float x0 = (d1 + a0) - q;
        if (rp.roadDist <= x0) {
            const float dd = x0 - rp.roadDist;
            out.targetPtOffset = dd < 30.0f ? (dd <= 7.0f ? 7.0f : dd) : 20.0f;
            sideOut(mm1RightEdge(*cur) - rSide - (next->alley ? 1.0f : 3.0f));
            return out;
        }
        const float w = (radius - r0) * s;
        const Vec3 centre{p.x + z.x * q - x.x * w, p.y + z.y * q - x.y * w, p.z + z.z * q - x.z * w};
        const Vec3 rel = in.pos - centre;
        const float lat = (x.x * s) * rel.x + (x.y * s) * rel.y + (x.z * s) * rel.z;
        const float ahead = (-z.x) * rel.x + (-z.y) * rel.y + (-z.z) * rel.z;
        const float phi = std::atan2(ahead, lat);
        const float rho = std::sqrt(lat * lat + ahead * ahead);
        out.roadDist = x0 + 2.0f * ((phi * q) / (s * a));
        const float look = (rho - radius) + (0.488f * (in.speed * in.speed)) / (23.76f * radius);
        out.targetPtOffset = look <= 7.0f ? 7.0f : (look < 20.0f ? look : 20.0f);
        return out; // DistToSide is not written in the arc
    }

    // No component: project onto the rail's current road.
    const Mm1RoadPos rp = mm1ProjectOffRoad(map, *cur, in.pos, in.pathId, rSide);
    out.vertex = rp.vertex;
    if (rp.found) {
        out.roadDist = rp.roadDist;
        out.hasRoadDist = true;
        out.distToSide = rp.distToSide;
        out.hasDistToSide = true;
    }
    out.targetPtOffset = 20.0f;
    return out;
}

} // namespace mm2::city
