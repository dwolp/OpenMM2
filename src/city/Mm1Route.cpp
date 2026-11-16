#include "city/Mm1Route.h"

#include <algorithm>
#include <cmath>

namespace mm2::city {

namespace {

const Mm1AiPath* pathAt(const Mm1AiMap& map, int id) {
    return id >= 0 && static_cast<std::size_t>(id) < map.paths.size() ? &map.paths[static_cast<std::size_t>(id)]
                                                                       : nullptr;
}
const Mm1AiIntersection* intAt(const Mm1AiMap& map, int id) {
    return id >= 0 && static_cast<std::size_t>(id) < map.intersections.size()
               ? &map.intersections[static_cast<std::size_t>(id)]
               : nullptr;
}
int wayPt(const Mm1RaceRoute& r, int i) {
    return i >= 0 && static_cast<std::size_t>(i) < r.wayPtIds.size() ? r.wayPtIds[static_cast<std::size_t>(i)] : -1;
}

// [a, a + 0.2 d, 0.3, 0.7, 0.8, b] as the constructor builds them (p0 - d * -f).
int addLink(Mm1AiMap& map, const Vec3& a, const Vec3& b) {
    const Vec3 d = b - a;
    auto at = [&](float f) { return Vec3{a.x - d.x * f, a.y - d.y * f, a.z - d.z * f}; };
    const Vec3 verts[6] = {a, at(-0.2f), at(-0.3f), at(-0.7f), at(-0.8f), b};
    const int id = static_cast<int>(map.paths.size());
    map.paths.push_back(mm1PathFromVerts(verts, id));
    map.paths.back().oncoming = id;
    map.paths.back().sink = -1; // wired to intersections by the caller
    map.paths.back().source = -1;
    return id;
}

} // namespace

int mm1AddIntersection(Mm1AiMap& map, const Vec3& pos) {
    Mm1AiIntersection i;
    i.id = static_cast<int>(map.intersections.size());
    i.position = pos;
    map.intersections.push_back(std::move(i));
    return map.intersections.back().id;
}

Mm1RoadPos mm1RoadDistance(const Mm1AiPath& path, const Vec3& pos, int row) {
    Mm1RoadPos r;
    const int n = path.vertexCount;
    if (n <= 2 || path.center.size() < static_cast<std::size_t>(n) || path.zAxis.size() < static_cast<std::size_t>(n) ||
        path.xAxis.size() < static_cast<std::size_t>(n))
        return r;
    const int rowCount = static_cast<int>(path.rows.size());
    row = std::min(row, static_cast<int>(path.numLanes)); // the asm caps the row at the path's lane count
    const bool haveRow = row >= 0 && row < rowCount && path.rows[static_cast<std::size_t>(row)].size() >= static_cast<std::size_t>(n);
    for (int i = 2; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const Vec3& base = haveRow ? path.rows[static_cast<std::size_t>(row)][ui] : path.center[ui];
        const Vec3 d = pos - base;
        const Vec3& z = path.zAxis[ui];
        const float dot = (z.x * d.x + z.y * d.y) + z.z * d.z;
        if (dot <= 0.0f)
            continue;
        const Vec3 dc = pos - path.center[ui];
        const Vec3& x = path.xAxis[ui];
        const float lateral = -((x.x * dc.x + x.y * dc.y) + x.z * dc.z);
        if (!((path.length - -5.0f) > std::abs(lateral)))
            continue;
        r.found = true;
        r.vertex = i;
        r.distToSide = lateral;
        const std::size_t base0 = static_cast<std::size_t>(row) * static_cast<std::size_t>(n);
        const std::size_t so = base0 + 1, si = base0 + ui;
        r.roadDist = (path.subSectionOffsets.size() > si ? path.subSectionOffsets[si] - path.subSectionOffsets[so]
                                                         : mm1CenterLength(path, 1, i)) -
                     dot;
        return r;
    }
    return r;
}

int mm1DetRdSegBetweenInts(const Mm1AiMap& map, int a, int b) {
    const Mm1AiIntersection* ia = intAt(map, a);
    const Mm1AiIntersection* ib = intAt(map, b);
    if (!ia || !ib)
        return -1;
    for (int p : ia->sinkPaths)
        if (std::find(ib->sourcePaths.begin(), ib->sourcePaths.end(), p) != ib->sourcePaths.end())
            return p;
    return -1;
}

Mm1RaceRoute mm1BuildRaceRoute(Mm1AiMap& map, const Mm1AiNav& nav, std::span<const Vec3> rows) {
    Mm1RaceRoute r;
    const int n = static_cast<int>(rows.size());
    if (n < 3)
        return r;
    r.wayPtIds.assign(static_cast<std::size_t>(n + 1), -1);
    // Rows 1 .. n-2: the first intersection in the room under the row.
    for (int i = 1; i < n - 1; ++i) {
        const int room = nav.room(rows[static_cast<std::size_t>(i)]);
        int id = -1;
        for (const Mm1Component& c : nav.components(room))
            if (c.type == 2) {
                id = c.id;
                break;
            }
        if (id < 0)
            return {};
        r.wayPtIds[static_cast<std::size_t>(i)] = id;
    }
    // The start link ends at the intersection MapComponent finds at row 1 (hint -1).
    const Mm1AiNav::Result first = nav.mapComponent(rows[1], 1, -1);
    r.startLink = addLink(map, rows[0], rows[1]);
    if (first.type == 2 && intAt(map, first.id)) {
        map.paths[static_cast<std::size_t>(r.startLink)].sink = first.id;
        map.intersections[static_cast<std::size_t>(first.id)].sourcePaths.push_back(r.startLink);
    }
    // The finish link: from the last waypoint intersection to a new one at the finish row.
    const int lastWp = r.wayPtIds[static_cast<std::size_t>(n - 2)];
    const Vec3 q0 = intAt(map, lastWp)->position;
    const Vec3 finish = rows[static_cast<std::size_t>(n - 1)];
    r.lastLink = addLink(map, q0, finish);
    const int finishInt = mm1AddIntersection(map, finish);
    map.paths[static_cast<std::size_t>(r.lastLink)].source = lastWp;
    map.paths[static_cast<std::size_t>(r.lastLink)].sink = finishInt;
    map.intersections[static_cast<std::size_t>(lastWp)].sinkPaths.push_back(r.lastLink);
    map.intersections[static_cast<std::size_t>(finishInt)].sourcePaths.push_back(r.lastLink);
    r.wayPtIds[static_cast<std::size_t>(n - 1)] = finishInt;
    // The link beyond: the finish to q0 + 1.25 (finish - q0), the asm's q0 - (finish - q0) * -1.25.
    const Vec3 d = finish - q0;
    const Vec3 beyond{q0.x - d.x * -1.25f, q0.y - d.y * -1.25f, q0.z - d.z * -1.25f};
    r.nLastLink = addLink(map, finish, beyond);
    const int beyondInt = mm1AddIntersection(map, beyond);
    map.paths[static_cast<std::size_t>(r.nLastLink)].source = finishInt;
    map.paths[static_cast<std::size_t>(r.nLastLink)].sink = beyondInt;
    map.intersections[static_cast<std::size_t>(finishInt)].sinkPaths.push_back(r.nLastLink);
    map.intersections[static_cast<std::size_t>(beyondInt)].sourcePaths.push_back(r.nLastLink);
    r.numWayPts = n;
    // The constructor checks that each pair of waypoint intersections has a road between them.
    for (int i = 1; i < n - 1; ++i)
        if (mm1DetRdSegBetweenInts(map, r.wayPtIds[static_cast<std::size_t>(i)], r.wayPtIds[static_cast<std::size_t>(i + 1)]) < 0)
            return {};
    return r;
}

bool mm1LocateWayPtFromRoad(const Mm1AiMap& map, const Mm1RaceRoute& route, Mm1RouteState& st, int roadId,
                            const Vec3& carPos) {
    const Mm1AiPath* road = pathAt(map, roadId);
    if (!road)
        return false;
    const int wp = wayPt(route, st.wayPtIdx);
    const int idx = st.wayPtIdx;
    auto onCur = [&]() {
        const Mm1AiPath* c = pathAt(map, st.cur);
        if (!c)
            return;
        const Mm1RoadPos p = mm1RoadDistance(*c, carPos, 0);
        st.vertex = p.vertex;
        if (p.found) {
            st.roadDist = p.roadDist;
            st.distToSide = p.distToSide;
        }
    };
    if (road->sink == wp) {
        st.cur = roadId;
        int a = wayPt(route, idx);
        int b = (idx == route.numWayPts - 2 && st.curLap < st.laps) ? wayPt(route, 1) : wayPt(route, idx + 1);
        st.next = mm1DetRdSegBetweenInts(map, a, b);
        if (road->oncoming != st.next)
            return true;
        // The next road is this road's oncoming one: the waypoint counts as passed.
        ++st.wayPtIdx;
        st.cur = road->oncoming;
        st.next = mm1DetRdSegBetweenInts(map, wayPt(route, st.wayPtIdx), wayPt(route, st.wayPtIdx + 1));
        onCur();
        return true;
    }
    if (road->source == wp) {
        st.cur = road->oncoming;
        st.next = mm1DetRdSegBetweenInts(map, wayPt(route, idx), wayPt(route, idx + 1));
        onCur();
        return true;
    }
    // A road out of the road's end that leads to the waypoint.
    if (const Mm1AiIntersection* end = intAt(map, road->sink)) {
        for (int p : end->sinkPaths) {
            const Mm1AiPath* pp = pathAt(map, p);
            if (pp && pp->sink == wp) {
                st.cur = roadId;
                st.next = p;
                return true;
            }
        }
    }
    // Or out of its start: turn round onto the oncoming road.
    if (const Mm1AiIntersection* start = intAt(map, road->source)) {
        for (int p : start->sinkPaths) {
            const Mm1AiPath* pp = pathAt(map, p);
            if (pp && pp->sink == wp) {
                st.cur = road->oncoming;
                st.next = p;
                onCur();
                return true;
            }
        }
    }
    return false; // the asm logs "not found"
}

bool mm1LocateWayPtFromInt(const Mm1AiMap& map, const Mm1RaceRoute& route, Mm1RouteState& st, int intersectionId) {
    const Mm1AiIntersection* i = intAt(map, intersectionId);
    if (!i)
        return false;
    const int wp = wayPt(route, st.wayPtIdx);
    for (int p : i->sinkPaths) {
        const Mm1AiPath* pp = pathAt(map, p);
        if (pp && pp->sink == wp) {
            st.next = p;
            return true;
        }
    }
    return false;
}

void mm1PlanRoute(const Mm1AiMap& map, const Mm1RaceRoute& route, Mm1RouteState& st, const Vec3& carPos) {
    if (st.compType == 2 && st.compIdx == wayPt(route, st.wayPtIdx)) {
        ++st.wayPtIdx;
        if (st.wayPtIdx == route.numWayPts - 1) {
            st.next = route.nLastLink;
            if (st.laps > st.curLap) {
                ++st.curLap;
                st.wayPtIdx = 1;
            }
        }
    }
    if (st.wayPtIdx == route.numWayPts - 1) {
        st.cur = route.lastLink;
        st.next = route.nLastLink;
        st.state = 5;
        const Mm1AiIntersection* fin = intAt(map, wayPt(route, st.wayPtIdx));
        const Mm1AiPath* link = pathAt(map, route.lastLink);
        if (!fin || !link)
            return;
        const float d = carPos.dist(fin->position);
        st.roadDist = mm1CenterLength(*link, 0, 5) - d;
        if (d < 10.0f)
            st.finished = true;
        return;
    }
    switch (st.compType) {
    case 0:
        mm1LocateWayPtFromRoad(map, route, st, st.cur, carPos);
        break;
    case 1:
        mm1LocateWayPtFromRoad(map, route, st, st.compIdx, carPos);
        break;
    case 2:
        mm1LocateWayPtFromInt(map, route, st, st.compIdx);
        break;
    default:
        break;
    }
}

} // namespace mm2::city
