#include "city/Mm1Racer.h"

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

} // namespace mm2::city
