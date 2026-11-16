#include "city/Mm1Racer.h"

#include <cmath>

namespace mm2::city {

Mm1RacerAi::Mm1RacerAi(Mm1AiMap& map, const Mm1AiNav& nav, std::span<const Vec3> rows, int laps,
                       const Mm1RailParams& rail)
    : m_map(map), m_nav(nav), m_route(mm1BuildRaceRoute(map, nav, rows)), m_rail(rail) {
    m_st.laps = laps > 0 ? laps : 1;
}

void Mm1RacerAi::component(const Vec3& pos, const Vec3& carBack, float speed, int pathHint) {
    Mm1OppRail rail;
    rail.cur = m_st.cur;
    rail.next = m_st.next;
    rail.params = m_rail;
    Mm1OppInput in;
    in.pos = pos;
    in.carBack = carBack;
    in.speed = speed;
    in.distToSide = m_st.distToSide;
    in.type = m_st.compType;
    in.lastType = m_lastType;
    in.pathId = pathHint;
    const Mm1OppResult r = mm1DetermineOppMapComponent(m_map, m_nav, rail, in);
    m_st.compType = r.type;
    m_st.compIdx = r.id;
    m_st.vertex = r.vertex;
    if (r.hasRoadDist)
        m_st.roadDist = r.roadDist;
    if (r.hasDistToSide)
        m_st.distToSide = r.distToSide;
    m_tpo = r.targetPtOffset;
    if (r.newCur >= 0)
        m_st.cur = r.newCur;
}

void Mm1RacerAi::init(const Vec3& pos, const Vec3& carBack, float speed) {
    if (!valid())
        return;
    const int link = m_route.startLink;
    m_st.wayPtIdx = 1;
    m_st.curLap = 1;
    m_st.cur = link;
    m_st.next = mm1DetRdSegBetweenInts(m_map, m_route.wayPtIds[1], m_route.wayPtIds[2]);
    m_lastType = -1;
    component(pos, carBack, speed, link);
    m_lastType = m_st.compType;
    if (m_st.compType == 1)
        m_st.cur = m_st.compIdx;
    // Reset: the waypoint index is the first whose intersection is the current road's end.
    if (const Mm1AiPath* c = m_map.paths.size() > static_cast<std::size_t>(m_st.cur) && m_st.cur >= 0
                                 ? &m_map.paths[static_cast<std::size_t>(m_st.cur)]
                                 : nullptr)
        for (int i = 1; i < m_route.numWayPts; ++i)
            if (c->sink == m_route.wayPtIds[static_cast<std::size_t>(i)]) {
                m_st.wayPtIdx = i;
                break;
            }
}

Vec3 Mm1RacerAi::update(const Vec3& pos, const Vec3& carBack, float speed) {
    int hint = -1;
    if (m_st.wayPtIdx > 1 && m_st.wayPtIdx < m_route.numWayPts - 1)
        hint = mm1DetRdSegBetweenInts(m_map, m_route.wayPtIds[static_cast<std::size_t>(m_st.wayPtIdx - 1)],
                                      m_route.wayPtIds[static_cast<std::size_t>(m_st.wayPtIdx)]);
    component(pos, carBack, speed, hint);
    mm1PlanRoute(m_map, m_route, m_st, pos);
    m_lastType = m_st.compType;
    const Mm1AiPath* cur = m_st.cur >= 0 && static_cast<std::size_t>(m_st.cur) < m_map.paths.size()
                               ? &m_map.paths[static_cast<std::size_t>(m_st.cur)]
                               : nullptr;
    const Mm1AiPath* next = m_st.next >= 0 && static_cast<std::size_t>(m_st.next) < m_map.paths.size()
                                ? &m_map.paths[static_cast<std::size_t>(m_st.next)]
                                : nullptr;
    if (!cur)
        return pos;
    return mm1CalcCopRailPosition(*cur, next, m_rail, pos, m_st.roadDist + m_tpo, m_st.distToSide).pos;
}

float Mm1RacerAi::brakeDemand(float speed) const {
    auto pathAt = [&](int id) -> const Mm1AiPath* {
        return id >= 0 && static_cast<std::size_t>(id) < m_map.paths.size() ? &m_map.paths[static_cast<std::size_t>(id)]
                                                                            : nullptr;
    };
    const Mm1AiPath* cur = pathAt(m_st.cur);
    const Mm1AiPath* next = pathAt(m_st.next);
    if (!cur || !next || speed <= 0.0f || cur->vertexCount < 3)
        return 0.0f;
    const int n = m_route.numWayPts;
    const float d = m_st.distToSide;
    // One turn: `from` onto `to`, `ahead` the distance to its road's end (less the arc's setback
    // is taken here), `base` the length already counted before `from`'s end.
    auto demand = [&](const Mm1AiPath& from, const Mm1AiPath& to, float a, float lengthToEnd, bool tcpo,
                      bool alley) -> float {
        const float s = a < 0.0f ? -1.0f : 1.0f;
        float rpre = mm1RightEdge(from) - d * s;
        if (tcpo && s == 1.0f)
            rpre -= m_rail.turnCenterPtOffset;
        const float r0 = rpre < 0.5f ? 0.5f : rpre;
        const float theta = std::abs((3.14f - std::abs(a)) * 0.5f);
        const float radius = r0 / (1.0f - std::sin(theta));
        float vc = std::sqrt(radius * 23.76f);
        if (alley)
            vc *= 0.5f;
        const float q = radius * std::cos(theta);
        const Vec3 p = mm1CalcTurnIntersection(from, to, static_cast<int>(s), m_rail.turnCenterPtOffset);
        const auto i = static_cast<std::size_t>(from.vertexCount - 2);
        const Vec3 vc3 = p - from.center[i];
        const Vec3& z = from.zAxis[i];
        const float d1 = ((-z.x) * vc3.x + (-z.y) * vc3.y) + (-z.z) * vc3.z;
        const float dist = ((d1 + lengthToEnd) - q) - m_st.roadDist;
        const float t = dist / speed;
        if (speed <= vc)
            return 0.0f;
        return (speed - vc) / (t * 23.76f);
    };
    const float a1 = mm1TurnAngle(*cur, *next);
    if (a1 > 0.5f || a1 < -0.5f)
        return demand(*cur, *next, a1, mm1CenterLength(*cur, 1, cur->vertexCount - 2), true, next->alley);
    if (m_st.wayPtIdx >= n - 1)
        return 0.0f;
    // The turn at the end of the next road: the road after it leads from the current waypoint
    // (the next road leaves the waypoint's intersection) or from the one after (it arrives).
    const int idx = m_st.wayPtIdx;
    auto wp = [&](int i) { return i >= 0 && static_cast<std::size_t>(i) < m_route.wayPtIds.size() ? m_route.wayPtIds[static_cast<std::size_t>(i)] : -1; };
    int from = wp(idx), to = wp(idx + 1);
    if (cur->sink == wp(idx)) {
        from = wp(idx + 1);
        to = wp(idx + 2);
    }
    const Mm1AiPath* next2 = pathAt(mm1DetRdSegBetweenInts(m_map, from, to));
    if (!next2 || next->vertexCount < 3)
        return 0.0f;
    const float a2 = mm1TurnAngle(*next, *next2);
    if (a2 <= 0.5f && a2 >= -0.5f)
        return 0.0f;
    const float len = mm1CenterLength(*cur, 1, cur->vertexCount - 2) + mm1CenterLength(*next, 1, next->vertexCount - 2);
    return demand(*next, *next2, a2, len, false, false);
}

} // namespace mm2::city
